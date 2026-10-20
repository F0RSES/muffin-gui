#pragma once

#include <QString>
#include <QStringList>

namespace zapret {

// Quoting rules for a Windows command LINE, as opposed to a command VECTOR.
//
// Two of the three ways this application starts something hand the operating system a single
// string rather than an argument list, and only one of them gets help:
//
//   * QProcess::setProgram/setArguments (WindowsBackend's live path) composes the string itself.
//     Qt's qt_create_commandline (src/corelib/io/qprocess_win.cpp) wraps every argument that
//     contains a space or a tab in quotes and escapes the quotes inside it, and it converts '/'
//     to '\' -- but for the program only, never for the arguments. Passing argv elements that are
//     already quoted therefore does not help: Qt escapes those quotes, the child's parser un-
//     escapes them, and the program receives a file name that literally begins with a '"'
//     character. A winws.exe told to read '"C:\Users\a name\lists\ip.txt"' reports that it cannot
//     access the file, which looks exactly like a quoting failure and is the opposite.
//     Strategy.argv is kept free of quotes for that reason; see StrategyParser::tokenize.
//
//   * ShellExecuteExW's lpParameters (PrivilegeHelper::runElevated) is a raw string that Qt never
//     sees, so nothing quotes it for us.
//
//   * What the GUI shows the user -- the launch line in the log and the copied command of a
//     profile row -- is also a string, and one that is only trustworthy if it reads as the
//     command Windows will actually run.
//
// So quoting belongs here, once, and in the exact shape the CRT's CommandLineToArgvW documents.
// Every function is pure text: no WinAPI, no Q_OS_WIN guard, which is what lets coretest assert
// them on a Linux host.

// Quote one token for inclusion in a command line.
//
// A token is left bare when it is non-empty and holds no space, no tab and no quote. Otherwise it
// is wrapped in quotes, with every inner quote preceded by a backslash and every run of backslashes
// that ends up in front of one of those quotes doubled -- doubling is what makes the quote escape
// survive both this quoting layer and the child's parser. A token that ends in backslashes has them
// doubled as well, because the closing quote would otherwise be escaped and the token would swallow
// the next one.
inline QString winQuoteArg(const QString &token)
{
    if (!token.isEmpty() && !token.contains(QLatin1Char(' ')) && !token.contains(QLatin1Char('\t'))
        && !token.contains(QLatin1Char('"')))
        return token;

    QString out;
    out.reserve(token.size() + 2);
    out += QLatin1Char('"');
    qsizetype backslashes = 0;
    for (qsizetype i = 0; i < token.size(); ++i) {
        const QChar c = token.at(i);
        if (c == QLatin1Char('\\')) {
            ++backslashes;
            continue;
        }
        if (c == QLatin1Char('"')) {
            // Every backslash still pending sits just before a quote, so it has to be doubled
            // first -- an odd number would eat the quote we are about to escape.
            for (; backslashes > 0; --backslashes)
                out += QLatin1String("\\\\");
            out += QLatin1String("\\\"");
            continue;
        }
        for (; backslashes > 0; --backslashes)
            out += QLatin1Char('\\');       // a backslash before an ordinary character is literal
        out += c;
    }
    for (; backslashes > 0; --backslashes)   // trailing run: doubled so it cannot escape the closer
        out += QLatin1String("\\\\");
    out += QLatin1Char('"');
    return out;
}

// The whole command line Qt would hand CreateProcessW for this program and argv -- program first,
// each argument quoted as needed. Qt wraps the program when its path holds a space and gives it
// native separators, and it normalizes nothing in the arguments; both are reproduced here so that
// what the GUI prints is what the child actually receives.
inline QString winComposeCommandLine(const QString &program, const QStringList &args)
{
    QStringList parts;
    parts.reserve(args.size() + 1);
    if (!program.isEmpty()) {
        QString prog = program;
        prog.replace(QLatin1Char('/'), QLatin1Char('\\'));
        if (prog.contains(QLatin1Char(' ')) && !prog.startsWith(QLatin1Char('"')))
            prog = QLatin1Char('"') + prog + QLatin1Char('"');
        parts << prog;
    }
    for (const QString &a : args)
        parts << winQuoteArg(a);
    return parts.join(QLatin1Char(' '));
}

// True for a token that names a path rather than a number, a protocol list or a name: a drive
// prefix, a UNC or rooted path, or something already carrying a native separator. Deliberately
// narrow -- winws flags also carry values like "80,443" and "fake,split", and a blanket
// '/'-to-'\' rewrite of those would corrupt an argument that is not a path at all.
inline bool winLooksLikePath(const QString &token)
{
    if (token.size() >= 2 && token.at(0).isLetter() && token.at(1) == QLatin1Char(':'))
        return true;                                    // C:/... or C:\...
    if (token.startsWith(QLatin1Char('\\')) || token.startsWith(QLatin1Char('/')))
        return true;                                    // \\server\share, /absolute
    return token.contains(QLatin1Char('\\'));
}

// A winws argument, either "--flag=C:\path" or a bare "C:\path", rewritten so the path part uses
// native separators. A batch file expands %~dp0 to backslashes and Qt does the same for the
// program, but Qt never rewrites an argument, so without this the child receives the mixed form
// "C:/Users/name/tree\lists\file.txt" that QFileInfo produces on Windows. Only the value is
// touched, only when it passes winLooksLikePath, and a flag name is never rewritten.
inline QString winNativePathArg(const QString &arg)
{
    auto swap = [](QString s) {
        s.replace(QLatin1Char('/'), QLatin1Char('\\'));
        return s;
    };

    const qsizetype eq = arg.indexOf(QLatin1Char('='));
    if (arg.startsWith(QLatin1Char('-')) && eq > 1) {
        const QString value = arg.mid(eq + 1);
        return winLooksLikePath(value) ? arg.left(eq + 1) + swap(value) : arg;
    }
    return winLooksLikePath(arg) ? swap(arg) : arg;
}

} // namespace zapret
