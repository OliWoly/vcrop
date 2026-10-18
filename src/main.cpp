#include "MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileOpenEvent>
#include <QIcon>
#include <QUrl>

namespace {

class App : public QApplication
{
public:
    using QApplication::QApplication;

    // Opens `path` in a window of its own, reusing a window that is still empty.
    void open(const QString &path)
    {
        MainWindow *window = nullptr;
        const auto widgets = topLevelWidgets();
        for (QWidget *w : widgets) {
            auto *candidate = qobject_cast<MainWindow *>(w);
            if (candidate && !candidate->hasMedia()) {
                window = candidate;
                break;
            }
        }
        if (!window) {
            window = new MainWindow;
            window->setAttribute(Qt::WA_DeleteOnClose);
        }
        window->show();
        window->raise();
        window->activateWindow();
        if (!path.isEmpty())
            window->openFile(path);
    }

protected:
    // Finder hands over files ("Open With") as events rather than arguments.
    bool event(QEvent *event) override
    {
        if (event->type() == QEvent::FileOpen) {
            open(static_cast<QFileOpenEvent *>(event)->file());
            return true;
        }
        return QApplication::event(event);
    }
};

} // namespace

int main(int argc, char *argv[])
{
    App app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("vcrop"));
    QApplication::setApplicationDisplayName(QStringLiteral("VCrop"));
    QApplication::setApplicationVersion(QStringLiteral(VCROP_VERSION));
    QApplication::setDesktopFileName(QStringLiteral("vcrop"));
#ifndef Q_OS_MACOS
    QApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("vcrop")));
#endif

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Crop and trim a video, then export it with FFmpeg."));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument(QStringLiteral("video"), QStringLiteral("Video file to open."), QStringLiteral("[video...]"));
    parser.process(app);

    const QStringList files = parser.positionalArguments();
    if (files.isEmpty())
        app.open(QString());
    for (const QString &file : files)
        app.open(QUrl::fromUserInput(file, QDir::currentPath(), QUrl::AssumeLocalFile).toLocalFile());

    return app.exec();
}
