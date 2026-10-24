#!/usr/bin/env node
/**
 * Preflight check for the polkit rule that packaging/postinst installs.
 *
 * Extracts the rules.d body from the maintainer script and evaluates it against a stub of the
 * polkit JS API (polkit.addRule / polkit.Result / action.lookup("program") / the subject fields
 * polkit(8) documents), then asserts a table of authorisation decisions.
 *
 * It exists because the security boundary here is a string comparison, and the failure mode of
 * getting it wrong is not a crash: `program` is the binary pkexec is about to exec, and the payload
 * this GUI downloads into the user's own home is one the user can overwrite with arbitrary bytes.
 * A rule that matches the payload by name suffix is therefore a local root, shipped through
 * `apt install`, and nothing at runtime would ever complain about it.
 *
 * A rule returning anything other than YES leaves the decision to the next rule and to polkit's own
 * default policy -- i.e. the user is prompted -- which is the safe outcome for every "abstain" row.
 *
 * Usage: node packaging/check-polkit-rule.js [path-to-postinst]
 * Exit status: 0 when every case checks out, 1 when at least one problem is printed.
 */
const fs = require('fs');

const scriptPath = process.argv[2] || 'packaging/postinst';
const sh = fs.readFileSync(scriptPath, 'utf8');

// The rule is written by a quoted heredoc, so nothing in it is expanded by the shell.
const m = sh.match(/^cat <<'RULE_EOF' > [^\n]*\n([\s\S]*?)\nRULE_EOF$/m);
if (!m) {
  console.error(`${scriptPath}: no heredoc rule between cat <<'RULE_EOF' and RULE_EOF`);
  process.exit(1);
}
const rule = m[1];

const Result = {
  YES: 'YES',
  NO: 'NO',
  AUTH_SELF: 'AUTH_SELF',
  AUTH_SELF_KEEP: 'AUTH_SELF_KEEP',
  AUTH_ADMIN: 'AUTH_ADMIN',
  AUTH_ADMIN_KEEP: 'AUTH_ADMIN_KEEP',
  NOT_HANDLED: 'NOT_HANDLED',
};

// The engine walks registered rules until one returns something other than NOT_HANDLED. There is
// one rule here, so capturing what addRule was handed is enough.
let registered = null;
const polkit = { Result, addRule(fn) { registered = fn; } };

try {
  new Function('polkit', rule)(polkit);
} catch (e) {
  console.error(`${scriptPath}: the generated rule does not parse: ${e.message}`);
  process.exit(1);
}
if (typeof registered !== 'function') {
  console.error(`${scriptPath}: the rule registered nothing with polkit.addRule`);
  process.exit(1);
}

// polkit(8), "The Action type": action.lookup(key) is undefined for a key with no value.
function decide(actionId, program, subject) {
  const action = { id: actionId, lookup: (k) => (k === 'program' ? program : undefined) };
  return registered(action, Object.assign({ pid: 1000, user: 'norman', groups: 'norman,sudo' }, subject));
}

const EXEC = 'org.freedesktop.policykit.exec';
const CONSOLE = { active: true, local: true };

// want === null means "not YES". want === 'YES' means the rule authorised it.
//
// The tree prefixes here are not invented: `systemTree` is read out of ZapretPaths::systemInstallDir()
// below and the case list is built from it, so a rename of the destination changes the cases the rule
// has to match rather than silently leaving a rule that authorises a directory nothing writes.
const SYSTEM_TREE = '/opt/muffin/zapret';
const cases = [
  // --- authorised: firewall binaries, exact and root-owned ---
  [EXEC, '/usr/sbin/iptables', CONSOLE, 'YES'],
  [EXEC, '/usr/sbin/ip6tables', CONSOLE, 'YES'],

  // --- authorised: payload under a tree a non-root user cannot rewrite ---
  [EXEC, '/opt/muffin/zapret/binaries/linux-x86_64/nfqws', CONSOLE, 'YES'],
  [EXEC, '/opt/muffin/zapret/linux/binaries/linux-x86_64/nfqws', CONSOLE, 'YES'],
  [EXEC, '/opt/muffin/zapret/nfq/nfqws', CONSOLE, 'YES'],        // what install_bin.sh builds
  [EXEC, '/opt/muffin/zapret/binaries/linux-x86_64/./nfqws', CONSOLE, 'YES'], // "." stays inside
  [EXEC, '/usr/local/zapret/linux/binaries/linux-x86_64/nfqws', CONSOLE, 'YES'],
  [EXEC, '/opt/zapret/linux/binaries/linux-x86_64/nfqws', CONSOLE, 'YES'],
  [EXEC, '/usr/local/zapret/nfqws', CONSOLE, 'YES'],            // install_bin.sh leaves it at the root
  [EXEC, '/usr/local/zapret/linux/binaries/linux-x86_64/./nfqws', CONSOLE, 'YES'], // "." stays inside the tree

  // --- refused: the helper's staging and backup siblings, which share the prefix but not the tree ---
  // /opt/muffin/zapret.new is the directory the promotion builds in BEFORE it is renamed into place,
  // and /opt/muffin/zapret.old the incumbent moved aside. Both are siblings of the authorised tree, not
  // inside it, and the rule's prefix test carries the trailing separator precisely so that they stay
  // outside. Authorising them would mean a half-built tree — bytes still being copied, the only moment
  // they are not yet root-owned — could be exec'd without a password.
  [EXEC, '/opt/muffin/zapret.new/binaries/linux-x86_64/nfqws', CONSOLE, null],
  [EXEC, '/opt/muffin/zapret.old/binaries/linux-x86_64/nfqws', CONSOLE, null],
  [EXEC, '/opt/muffin/zapretx/binaries/linux-x86_64/nfqws', CONSOLE, null],
  [EXEC, '/opt/muffin/nfqws', CONSOLE, null],                   // the namespace is not the tree

  // --- refused: the root helper itself, which is the whole point of the split ---
  // /usr/lib/muffin/muffin-install-helper is the program the GUI asks pkexec to run in order to put
  // bytes this application downloaded INTO the passwordless tree. Authorising it would make the
  // passwordless tree writable by an unprivileged user with one more passwordless pkexec — the exact
  // exploit the promotion's chown/chmod steps exist to prevent — so promoting costs a human approval.
  [EXEC, '/usr/lib/muffin/muffin-install-helper', CONSOLE, null],
  [EXEC, '/usr/lib/muffin/muffin-install-helper', { active: true, local: true, inServer: true }, null],
  [EXEC, '/usr/bin/Muffin', CONSOLE, null],                     // the GUI is never a pkexec target

  // --- refused: the payload wherever the GUI actually puts it (user-owned => attacker-owned) ---
  [EXEC, '/home/norman/.local/share/muffin/zapret/binaries/linux-x86_64/nfqws', CONSOLE, null],
  [EXEC, '/home/norman/.local/share/Muffin/Muffin/zapret/nfq/nfqws', CONSOLE, null],
  [EXEC, '/home/norman/Documents/zapret/linux/binaries/linux-x86_64/nfqws', CONSOLE, null],
  [EXEC, '/home/norman/Downloads/zapret/linux/binaries/linux-x86_64/nfqws', CONSOLE, null],
  [EXEC, '/tmp/nfqws', CONSOLE, null],
  [EXEC, '/tmp/evil/nfqws', CONSOLE, null],
  [EXEC, '/dev/shm/nfqws', CONSOLE, null],
  [EXEC, '/usr/bin/nfqws', CONSOLE, null],

  // --- refused: a name that is not the payload, or not at the pinned path ---
  [EXEC, '/usr/local/zapret/linux/binaries/linux-x86_64/nfqws-old', CONSOLE, null],
  [EXEC, '/usr/local/zapret/nfqws.d/nfqwsx', CONSOLE, null],
  [EXEC, '/usr/local/zapretx/linux/binaries/linux-x86_64/nfqws', CONSOLE, null], // base must be a directory
  [EXEC, '/tmp/usr/local/zapret/linux/binaries/linux-x86_64/nfqws', CONSOLE, null], // prefix anchored at 0
  [EXEC, '/opt/zapret/../../tmp/x/nfqws', CONSOLE, null],                        // traversal before canonicalisation
  [EXEC, '/usr/local/zapret/../local/zapret/nfqws', CONSOLE, null],

  // --- refused: the firewall wrapper and every other shell / xtables neighbour ---
  [EXEC, '/bin/sh', CONSOLE, null],           // portable mode's firewall phases: still prompted
  [EXEC, '/usr/bin/sh', CONSOLE, null],
  [EXEC, '/bin/dash', CONSOLE, null],
  [EXEC, '/bin/bash', CONSOLE, null],
  [EXEC, '/usr/sbin/iptables-restore', CONSOLE, null],
  [EXEC, '/usr/sbin/iptables-save', CONSOLE, null],
  [EXEC, '/usr/sbin/nft', CONSOLE, null],
  [EXEC, '/usr/bin/pkexec', CONSOLE, null],
  [EXEC, '/usr/bin/rm', CONSOLE, null],
  [EXEC, 'nfqws', CONSOLE, null],             // a bare name is not a resolved path
  [EXEC, '', CONSOLE, null],
  [EXEC, undefined, CONSOLE, null],
  [EXEC, null, CONSOLE, null],

  // --- refused: anything that is not a console session, or not this action ---
  [EXEC, '/usr/sbin/iptables', { active: false, local: true }, null],
  [EXEC, '/usr/sbin/iptables', { active: true, local: false }, null],
  [EXEC, '/usr/sbin/iptables', {}, null],
  [EXEC, '/opt/zapret/linux/binaries/linux-x86_64/nfqws', { active: false, local: false }, null],
  ['org.freedesktop.NetworkManager.enable-disable-network', '/usr/sbin/iptables', CONSOLE, null],
  ['org.freedesktop.login1.power-off', '/opt/zapret/nfqws', CONSOLE, null],
];
let bad = 0;
for (const [id, program, subject, want] of cases) {
  let got;
  try {
    got = decide(id, program, subject);
  } catch (e) {
    console.log(`FAIL  ${id} ${JSON.stringify(program)} threw: ${e.message}`);
    bad++;
    continue;
  }
  // Abstaining is expressed as NOT_HANDLED or undefined; both leave the default policy in place.
  const abstained = got === undefined || got === Result.NOT_HANDLED;
  const ok = want === 'YES' ? got === Result.YES : abstained;
  if (!ok) bad++;
  console.log(
    `${ok ? 'PASS' : 'FAIL'}  ${want === 'YES' ? 'allow  ' : 'ask    '}  ` +
    `${String(JSON.stringify(program)).padEnd(64)} ${JSON.stringify(subject).padEnd(33)} -> ${String(got)}`
  );
  // Overriding another administrator's denial, or dictating an auth style, is never this rule's job.
  if (got !== undefined && got !== Result.NOT_HANDLED && got !== Result.YES) {
    console.log(`FAIL    rule returned ${got}: it must only ever return YES or abstain`);
    bad++;
  }
}

// Portability claim: this is evaluated by the JS engine inside polkitd, which on an LTS desktop may
// predate ES6. Guard the claim instead of trusting the comment that makes it.
const es6 = rule.match(/=>|\bconst\b|\blet\b|startsWith|endsWith|includes|\bpadEnd\b|`/g);
if (es6) {
  console.log(`FAIL  rule uses syntax older polkit may not evaluate: ${Array.from(new Set(es6)).join(' ')}`);
  bad++;
}

// polkitd evaluates the whole file as top-level JavaScript source, so shell comments (`#`) are a
// SyntaxError, not a comment: polkit logs "Error evaluating rule" and the file is silently skipped
// for the entire boot. `new Function('polkit', rule)` above would not have caught this because
// Function wraps in a body where the parse is more permissive only about ASI, not about `#`, so
// we already get a parse failure here -- but check by regex too, and name the offending lines, so
// the next person to edit the rule sees what to fix instead of just "does not parse".
const shellComments = rule.split('\n')
  .map((line, i) => [i + 1, line])
  .filter(([, line]) => /^\s*#(?!\/)/.test(line));
if (shellComments.length) {
  console.log(`FAIL  rule uses shell comments (JavaScript engine will reject the whole file): ` +
    shellComments.map(([n, l]) => `${n}:${l.trim()}`).join(' | '));
  bad++;
}

// The rule must be removed again, or an uninstall leaves iptables passwordless on a machine with no
// reason to run it. postinst names it, postrm has to delete it, and only on a real removal.
if (!/49-muffin-nopasswd\.rules/.test(sh)) {
  console.log(`FAIL  ${scriptPath} does not name 49-muffin-nopasswd.rules`);
  bad++;
}
const postrmPath = scriptPath.replace(/postinst$/, 'postrm');
if (fs.existsSync(postrmPath)) {
  const postrm = fs.readFileSync(postrmPath, 'utf8');
  if (!/rm -f[^\n]*49-muffin-nopasswd\.rules/.test(postrm)) {
    console.log(`FAIL  ${postrmPath} does not delete the rule it installed`);
    bad++;
  }
  if (!/case "\$1" in[\s\S]{0,40}remove\|purge\)/.test(postrm)) {
    console.log(`FAIL  ${postrmPath} must delete only on remove|purge, never mid-upgrade`);
    bad++;
  }
} else {
  console.log(`FAIL  ${postrmPath} is missing; the rule would outlive the package`);
  bad++;
}

// Installing must not depend on polkit being present: a server or container has no rules.d, and a
// failed `apt install` there is worse than an unauthorised desktop.
if (!/\[\s*! -d "\$RULES_DIR"\s*\]/.test(sh)) {
  console.log(`FAIL  ${scriptPath} does not skip when /etc/polkit-1/rules.d is absent`);
  bad++;
}

// ---- the three files that must agree on two paths ----
//
// The rule, the C++ that composes the promotion, and the CMake that installs the helper each name a
// path, and the security property only holds while they are the same path. A comment saying so is
// worth nothing here, so each literal is read out of its source and compared against the rule that
// was just evaluated — the drift is a CI failure instead of a passwordless tree nothing wrote.
const repoRoot = __dirname.replace(/[/\\]packaging[/\\]?$/, '');
const read = (rel) => {
  try {
    return fs.readFileSync(repoRoot + '/' + rel, 'utf8');
  } catch (e) {
    console.log(`FAIL  cannot read ${rel} to cross-check the rule: ${e.message}`);
    bad++;
    return '';
  }
};

// 1. ZapretPaths::systemInstallDir() — the destination the helper writes and the rule exempts.
//
// The POSIX literal is the one this check exists to pin, and systemInstallDir() now carries a second
// QStringLiteral on the Windows branch (the "C:/Program Files" default). Anchoring on a leading '/' is
// what keeps the two apart: the rule and the tree it exempts are POSIX-only, and a destination that
// begins with a drive letter is not something polkit can ever match.
const pathsCpp = read('src/core/ZapretPaths.cpp');
const treeMatch = pathsCpp.match(
  /ZapretPaths::systemInstallDir\(\)[\s\S]*?return QStringLiteral\("(\/[^"]+)"\);/);
if (!treeMatch) {
  console.log('FAIL  src/core/ZapretPaths.cpp: no QStringLiteral destination in systemInstallDir()');
  bad++;
} else {
  const tree = treeMatch[1];
  if (tree !== SYSTEM_TREE) {
    console.log(`FAIL  systemInstallDir() is ${tree} but the auth cases assume ${SYSTEM_TREE}: ` +
      'update the case list and the rule together');
    bad++;
  }
  // The rule must carry the destination WITH its trailing separator. Without it the prefix test
  // matches /opt/muffin/zapret.new — the staging directory — as well as the tree, which is exactly the
  // difference between "a tree nothing but root can write" and "whatever the last copy in progress is".
  if (!rule.includes(`"${tree}/"`)) {
    console.log(`FAIL  the rule does not exempt the tree the code installs into: no "${tree}/" prefix`);
    bad++;
  }
  // And it must not carry the same destination WITHOUT the separator: a bare "${tree}" prefix test
  // would authorise the staging and backup siblings too.
  if (new RegExp(`indexOf\\("${tree}"\\)`).test(rule)) {
    console.log(`FAIL  the rule tests "${tree}" without a trailing separator: the .new/.old siblings ` +
      'would be authorised');
    bad++;
  }
}

// 2. The helper: its name and installed directory, then the CMake DESTINATION that puts it there.
const helperCpp = read('src/core/MuffinInstallHelper.cpp');
const nameMatch = helperCpp.match(/programName\(\)[\s\S]{0,120}?return QStringLiteral\("([^"]+)"\);/);
const dirMatch = helperCpp.match(/installedProgram\(\)[\s\S]{0,900}?QStringLiteral\("([^"]+)"\)\s*\+\s*programName/);
const cmake = read('CMakeLists.txt');
const destMatch = cmake.match(/install\(TARGETS\s+muffin-install-helper\s+RUNTIME\s+DESTINATION\s+(\S+)\)/);
if (!nameMatch || !dirMatch || !destMatch) {
  console.log('FAIL  the helper path is not derivable: ' +
    `programName=${nameMatch ? 'ok' : 'no'}, installedProgram=${dirMatch ? 'ok' : 'no'}, ` +
    `cmake DESTINATION=${destMatch ? 'ok' : 'no'}`);
  bad++;
} else {
  // CMakeLists.txt installs under a /usr prefix (CPACK_SET_DESTDIR is unset and CPACK_PACKAGING_
  // INSTALL_PREFIX defaults to /usr), so the packaged path is /usr/<DESTINATION>/<programName> and
  // installedProgram() must spell exactly that.
  const installed = dirMatch[1] + nameMatch[1];
  const packaged = '/usr/' + destMatch[1] + '/' + nameMatch[1];
  if (installed !== packaged) {
    console.log(`FAIL  installedProgram() is ${installed} but CMake installs it to ${packaged}`);
    bad++;
  }
  // The decisive half: the helper must NOT be authorised. Read as a path from CMake rather than
  // assumed, because "the rule refuses the helper" is only a fact if it is the same path the
  // package ships.
  if (decide(EXEC, packaged, CONSOLE) === Result.YES) {
    console.log(`FAIL  the rule authorises ${packaged}: promotion would be passwordless`);
    bad++;
  }
  if (!rule.includes(nameMatch[1])) {
    console.log(`FAIL  the rule does not name ${nameMatch[1]}, so nothing documents that it is refused`);
    bad++;
  }
}

// 3. The sibling names the rule must keep outside the prefix: staging and backup, read from the code
//    that creates them rather than from this file's opinion about what they should be called.
for (const suffix of ['.new', '.old']) {
  if (!new RegExp(`QStringLiteral\\("${suffix.replace('.', '\\.')}\"\\)`).test(helperCpp) &&
      !new RegExp(`\\+\\s*QStringLiteral\\("${suffix.replace('.', '\\.')}\"\\)`).test(helperCpp)) {
    console.log(`FAIL  MuffinInstallHelper no longer builds ${SYSTEM_TREE}${suffix}; ` +
      'the staging/backup refusal cases here are stale');
    bad++;
  }
  if (decide(EXEC, `${SYSTEM_TREE}${suffix}/nfqws`, CONSOLE) === Result.YES) {
    console.log(`FAIL  the rule authorises ${SYSTEM_TREE}${suffix}: a half-built tree would be root-runnable`);
    bad++;
  }
}

// 4. postrm has to clean the promoted tree up as well as the rule: it is not in the package's file
//    list, so nothing else removes it and the authorisation would outlive the application.
if (fs.existsSync(postrmPath)) {
  const postrm = fs.readFileSync(postrmPath, 'utf8');
  if (!/purge\)[\s\S]{0,400}?rm -rf[^\n]*\/opt\/muffin\/zapret/.test(postrm)) {
    console.log(`FAIL  ${postrmPath} does not delete ${SYSTEM_TREE} on purge`);
    bad++;
  }
}

console.log(bad === 0
  ? `\nALL ${cases.length} AUTH CASES PASS`
  : `\n${bad} FAILURE(S)`);
process.exit(bad === 0 ? 0 : 1);

