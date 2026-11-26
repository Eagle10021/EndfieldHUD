// EndfieldHUD for Linux: a full-screen layer-shell overlay on Wayland showing the original HUD.
//   endfieldhud                      run in the background; Ctrl+Tab or the tray icon opens the HUD
//   endfieldhud toggle|open|close|quit   control the running instance
// Options: --resources <dir>   resources directory of an official release
//          --passive           for testing: no keyboard or pointer input and no shortcut
//          --preview <seconds> show once, passively, then exit
#include "graphics_host.hpp"
#include "hud_shell.hpp"
#include "presenter_gl.hpp"

#include <KGlobalAccel>
#include <KWindowEffects>
#include <LayerShellQt/Window>

#include <QAction>
#include <QApplication>
#include <QChronoTimer>
#include <QCursor>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusInterface>
#include <QDateTime>
#include <QIcon>
#include <QKeyEvent>
#include <QLocale>
#include <QMenu>
#include <QMouseEvent>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QOpenGLFunctions>
#include <QScreen>
#include <QProcess>
#include <QSurfaceFormat>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QWheelEvent>
#include <QWindow>

#include <chrono>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>

namespace {
namespace linux_port = endfield::linux_port;
namespace gpu = endfield::native;
namespace core = endfield::core;

const QString serviceName = QStringLiteral("org.endfieldhud.HUD");
const QString objectPath = QStringLiteral("/HUD");

double now() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

class Overlay final : public QWindow, public linux_port::GraphicsHost {
public:
    Overlay(const std::filesystem::path& resources, bool passive, bool once) : once_(once) {
        setSurfaceType(QSurface::OpenGLSurface);
        setFlags(passive ? Qt::Window | Qt::FramelessWindowHint | Qt::WindowTransparentForInput : Qt::Window | Qt::FramelessWindowHint);
        setTitle(QStringLiteral("EndfieldHUD"));
        auto* layer = LayerShellQt::Window::get(this);
        layer->setScope(QStringLiteral("endfieldhud"));
        layer->setLayer(LayerShellQt::Window::LayerOverlay);
        layer->setAnchors(LayerShellQt::Window::Anchors(LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorBottom |
                                                        LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight));
        layer->setExclusiveZone(-1); // covers panels
        layer->setKeyboardInteractivity(passive ? LayerShellQt::Window::KeyboardInteractivityNone : LayerShellQt::Window::KeyboardInteractivityExclusive);
        layer->setActivateOnShow(!passive);
        layer->setWantsToBeOnActiveScreen(true);
        // ENDFIELDHUD_SCREEN names an output (for example eDP-1) to open on instead of the active one.
        for (QScreen* output : QGuiApplication::screens())
            if (output->name() == qEnvironmentVariable("ENDFIELDHUD_SCREEN")) {
                layer->setWantsToBeOnActiveScreen(false); layer->setScreen(output); setScreen(output);
            }

        context_.setFormat(QSurfaceFormat::defaultFormat());
        if (!context_.create()) throw std::runtime_error("Cannot create an OpenGL context");
        offscreen_.setFormat(context_.format()); offscreen_.create();
        makeCurrent();

        rasterizer_ = std::make_unique<gpu::LayerRasterizer>();
        shell_ = std::make_unique<linux_port::HudShell>(resources, *rasterizer_, core::Language::english);
        gpu::RendererOptions options;
        options.target = gpu::RenderTarget::composition;
        options.shaderPath = SHADER_DIR;
        renderer_.initialize(static_cast<linux_port::GraphicsHost*>(this), 64, 64, options);

        makeCurrent();
        graphics_ = reinterpret_cast<const char*>(context_.functions()->glGetString(GL_RENDERER));
        if (bool set = false; qEnvironmentVariableIntValue("ENDFIELDHUD_FPS", &set) >= 0 && set)
            frameLimit_ = qEnvironmentVariableIntValue("ENDFIELDHUD_FPS");
        pacer_.setSingleShot(true); pacer_.setTimerType(Qt::PreciseTimer);
        QObject::connect(&pacer_, &QChronoTimer::timeout, this, [this] { scheduled_ = false; render(); });
        const QPixmap cursor(QString::fromStdString((resources / "shell" / "cursor.png").string()));
        if (!cursor.isNull()) setCursor(QCursor(cursor, 0, 0));
    }
    ~Overlay() override {
        makeCurrent();
        shell_.reset(); renderer_.reset(); presenter_.release();
    }

    void openHud() {
        if (isVisible() && !closing_) return;
        closing_ = false; openPending_ = true;
        if (!isVisible()) { frames_ = late_ = 0; poseSeconds_ = drawSeconds_ = swapSeconds_ = slowest_ = lastFrame_ = 0; show(); }
        schedule();
    }
    void closeHud() {
        if (!isVisible() || closing_) return;
        closing_ = true;
        if (openPending_) { openPending_ = false; hide(); return; }
        shell_->close(now());
        schedule();
    }
    void toggle() { if (isVisible() && !closing_) closeHud(); else openHud(); }

    void makeCurrent() override {
        QSurface* surface = isExposed() ? static_cast<QSurface*>(this) : &offscreen_;
        if (QOpenGLContext::currentContext() == &context_ && context_.surface() == surface) return;
        if (!context_.makeCurrent(surface)) throw std::runtime_error("Cannot activate the OpenGL context");
    }
    void present(unsigned texture, unsigned width, unsigned height) override {
        // 0.63 is the original default background darkness.
        presenter_.draw(context_.defaultFramebufferObject(), texture, width, height, {shell_->backdropOpacity(frameTime_), 0.63});
    }

protected:
    bool event(QEvent* e) override {
        if (e->type() == QEvent::Leave && shell_) shell_->pointer(std::nullopt, now());
        return QWindow::event(e);
    }
    void exposeEvent(QExposeEvent*) override {
        if (!isExposed()) return;
        if (!qEnvironmentVariableIsSet("ENDFIELDHUD_NO_BLUR")) KWindowEffects::enableBlurBehind(this, true); // needs the Wayland surface
        schedule();
    }
    void keyPressEvent(QKeyEvent* e) override {
        const bool summon = e->key() == Qt::Key_Tab && e->modifiers() == Qt::ControlModifier;
        if (e->key() == Qt::Key_Escape || summon) closeHud();
    }
    void mouseMoveEvent(QMouseEvent* e) override { shell_->pointer(point(e->position()), now()); }
    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() == Qt::LeftButton && !closing_ && shell_->press(point(e->position()), now())) closeHud();
    }
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton || closing_) return;
        const auto action = shell_->release(point(e->position()), now());
        if (action.empty()) return;
        if (action == "close") closeHud();
        else if (action == "quit") QCoreApplication::quit();
        else if (action == "terminal") { closeHud(); QProcess::startDetached(QStringLiteral("kitty"), {}, qEnvironmentVariable("HOME")); }
        else std::cout << "Module not available on Linux yet: " << action << std::endl;
    }
    void wheelEvent(QWheelEvent* e) override {
        const double steps = e->angleDelta().y() / 120.0;
        if (steps != 0 && !closing_) shell_->wheel(point(e->position()), steps, now());
    }

private:
    static core::Point point(const QPointF& p) { return {p.x(), p.y()}; }
    void render() {
        if (!isVisible() || !isExposed()) return;
        try {
            const double scale = devicePixelRatio();
            const unsigned w = unsigned(std::lround(width() * scale)), h = unsigned(std::lround(height() * scale));
            if (w < 64 || h < 64) { schedule(); return; }
            frameTime_ = now();
            makeCurrent();
            if (w != width_ || h != height_ || scale != scale_) {
                width_ = w; height_ = h; scale_ = scale;
                renderer_.resize(w, h); shell_->resize({w, h, scale}, frameTime_);
            }
            updateClock();
            if (openPending_) { openPending_ = false; openedAt_ = frameTime_; shell_->open(frameTime_, random_()); }
            const double begin = now();
            if (!shell_->frame(renderer_, frameTime_)) {
                if (!shell_->shown()) { hideNow(); return; }
                schedule(); return;
            }
            const double posed = now();
            renderer_.draw(true);
            const double drawn = now();
            context_.swapBuffers(this);
            const double swapped = now();
            poseSeconds_ += posed - begin; drawSeconds_ += drawn - posed; swapSeconds_ += swapped - drawn;
            slowest_ = std::max(slowest_, frames_ > 30 && lastFrame_ > 0 ? frameTime_ - lastFrame_ : 0);
            const double target = frameLimit_ > 0 ? std::min<double>(frameLimit_, screen()->refreshRate()) : screen()->refreshRate();
            if (frames_ > 30 && lastFrame_ > 0 && frameTime_ - lastFrame_ > 1.5 / target) ++late_;
            lastFrame_ = frameTime_;
            ++frames_;
            schedule();
        } catch (const std::exception& error) {
            std::cerr << "EndfieldHUD: " << error.what() << std::endl;
            hideNow();
            if (once_) QCoreApplication::exit(1);
        }
    }
    // Frames start on a fixed schedule at the frame limit; swapBuffers then waits only if the
    // compositor has not taken the previous frame. Waiting for the compositor before drawing
    // (requestUpdate) misses refreshes on fast screens. A limit of 0 follows the screen.
    void schedule() {
        if (scheduled_) return;
        scheduled_ = true;
        const double time = now();
        due_ = frameLimit_ > 0 ? std::max(time, due_ + 1.0 / frameLimit_) : time;
        pacer_.setInterval(std::chrono::nanoseconds(static_cast<std::int64_t>((due_ - time) * 1e9)));
        pacer_.start();
    }
    void hideNow() {
        const double seconds = frameTime_ - openedAt_;
        if (frames_ > 1 && seconds > 0 && qEnvironmentVariableIsSet("ENDFIELDHUD_TIMING"))
            std::cout << frames_ << " frames in " << seconds << " s (" << frames_ / seconds << " per second, screen " << screen()->refreshRate()
                      << " Hz); per frame: pose " << 1000 * poseSeconds_ / frames_ << " ms, draw " << 1000 * drawSeconds_ / frames_ << " ms, swap "
                      << 1000 * swapSeconds_ / frames_ << " ms; after the opening: " << late_ << " late, longest gap " << 1000 * slowest_ << " ms; "
                      << graphics_ << " on " << screen()->name().toStdString() << std::endl;
        closing_ = false; openPending_ = false;
        hide();
        if (once_) QCoreApplication::quit();
    }
    void updateClock() {
        const auto time = QDateTime::currentDateTime();
        const qint64 second = time.toSecsSinceEpoch();
        if (second == clockSecond_) return;
        clockSecond_ = second;
        const QLocale english(QLocale::English);
        const QString date = english.toString(time.date(), QStringLiteral("ddd")).toUpper() + QLatin1Char(' ') + english.toString(time.date(), QStringLiteral("MMM d"));
        shell_->setReading(time.toString(QStringLiteral("HH:mm:ss")).toStdString(), date.toStdString(), "CTRL + TAB");
    }

    QOpenGLContext context_;
    QOffscreenSurface offscreen_;
    gpu::Renderer renderer_;
    QChronoTimer pacer_;
    int frameLimit_{60};
    double due_{};
    linux_port::Presenter presenter_;
    std::unique_ptr<gpu::LayerRasterizer> rasterizer_;
    std::unique_ptr<linux_port::HudShell> shell_;
    std::mt19937_64 random_{std::random_device{}()};
    unsigned width_{}, height_{};
    double scale_{}, frameTime_{}, openedAt_{};
    qint64 clockSecond_{};
    std::uint64_t frames_{}, late_{};
    double poseSeconds_{}, drawSeconds_{}, swapSeconds_{}, slowest_{}, lastFrame_{};
    std::string graphics_;
    bool once_{}, openPending_{}, closing_{}, scheduled_{};
};

// The session-bus interface of the running instance.
class Control final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.endfieldhud.HUD")
public:
    explicit Control(Overlay& overlay) : overlay_(overlay) {}
public Q_SLOTS:
    Q_SCRIPTABLE void Toggle() { overlay_.toggle(); }
    Q_SCRIPTABLE void Open() { overlay_.openHud(); }
    Q_SCRIPTABLE void Close() { overlay_.closeHud(); }
    Q_SCRIPTABLE void Quit() { QCoreApplication::quit(); }
private:
    Overlay& overlay_;
};

bool send(const QString& method) {
    QDBusInterface running(serviceName, objectPath, serviceName, QDBusConnection::sessionBus());
    return running.isValid() && running.call(method).type() != QDBusMessage::ErrorMessage;
}
} // namespace

int main(int argc, char** argv) {
    QSurfaceFormat format;
    format.setRenderableType(QSurfaceFormat::OpenGL);
    format.setVersion(4, 6);
    format.setProfile(QSurfaceFormat::CoreProfile);
    format.setAlphaBufferSize(8);
    format.setDepthBufferSize(0);
    format.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("endfieldhud"));
    QGuiApplication::setApplicationDisplayName(QStringLiteral("EndfieldHUD"));
    QGuiApplication::setDesktopFileName(QStringLiteral("org.endfieldhud.linux"));
    QApplication::setQuitOnLastWindowClosed(false);

    QString resources = qEnvironmentVariable("ENDFIELDHUD_RESOURCES");
    double preview = 0;
    bool passive = false;
    QString command;
    const QStringList arguments = application.arguments();
    for (qsizetype i = 1; i < arguments.size(); ++i) {
        const QString& a = arguments[i];
        if (a == QLatin1String("--resources") && i + 1 < arguments.size()) resources = arguments[++i];
        else if (a == QLatin1String("--preview") && i + 1 < arguments.size()) preview = arguments[++i].toDouble();
        else if (a == QLatin1String("--passive")) passive = true;
        else if (command.isEmpty() && (a == QLatin1String("toggle") || a == QLatin1String("open") || a == QLatin1String("close") || a == QLatin1String("quit"))) command = a;
        else { std::cerr << "usage: endfieldhud [toggle|open|close|quit] [--resources <dir>] [--passive] [--preview <seconds>]\n"; return 2; }
    }
    if (!command.isEmpty()) {
        const QString method = command.left(1).toUpper() + command.mid(1);
        if (send(method)) return 0;
        std::cerr << "EndfieldHUD is not running.\n";
        return 1;
    }
    if (resources.isEmpty()) resources = qEnvironmentVariable("HOME") + QStringLiteral("/.local/share/endfieldhud-linux/resources");
    if (QGuiApplication::platformName() != QLatin1String("wayland")) { std::cerr << "EndfieldHUD needs a Wayland session.\n"; return 1; }

    auto bus = QDBusConnection::sessionBus();
    if (preview <= 0 && bus.interface()->isServiceRegistered(serviceName)) return send(QStringLiteral("Open")) ? 0 : 1;

    try {
        // The resource readers refuse links, so a linked resources directory is resolved first.
        const auto root = std::filesystem::canonical(std::filesystem::path(resources.toStdString()));
        resources = QString::fromStdString(root.string());
        Overlay overlay(root, passive || preview > 0, preview > 0);
        if (preview > 0) {
            QTimer::singleShot(0, &application, [&] { overlay.openHud(); });
            QTimer::singleShot(int(preview * 1000), &application, [&] { overlay.closeHud(); });
            QTimer::singleShot(int(preview * 1000) + 5000, &application, [] { QCoreApplication::exit(1); });
            return application.exec();
        }
        Control control(overlay);
        if (!bus.registerObject(objectPath, &control, QDBusConnection::ExportScriptableSlots) || !bus.registerService(serviceName)) {
            std::cerr << "Cannot register on the session bus.\n";
            return 1;
        }

        // Ctrl+Tab, as on Windows; System Settings > Shortcuts can change it.
        QAction summon(QStringLiteral("Open or close the HUD"));
        summon.setObjectName(QStringLiteral("toggle"));
        if (!passive) {
            const QList<QKeySequence> keys{QKeySequence(Qt::CTRL | Qt::Key_Tab)};
            KGlobalAccel::self()->setDefaultShortcut(&summon, keys);
            KGlobalAccel::self()->setShortcut(&summon, keys);
            QObject::connect(&summon, &QAction::triggered, &application, [&] { overlay.toggle(); });
            if (KGlobalAccel::self()->shortcut(&summon).isEmpty()) std::cerr << "Ctrl+Tab could not be registered; another action may hold it.\n";
        }

        QSystemTrayIcon tray(QIcon(resources + QStringLiteral("/common/application-icons/alphaFour-tray.png")));
        QMenu menu;
        QObject::connect(menu.addAction(QStringLiteral("Open HUD")), &QAction::triggered, &application, [&] { overlay.openHud(); });
        QObject::connect(menu.addAction(QStringLiteral("Quit EndfieldHUD")), &QAction::triggered, &application, [] { QCoreApplication::quit(); });
        QObject::connect(&tray, &QSystemTrayIcon::activated, &application, [&](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger) overlay.toggle();
        });
        tray.setContextMenu(&menu);
        tray.setToolTip(QStringLiteral("EndfieldHUD"));
        tray.show();
        std::cout << "EndfieldHUD is running. Ctrl+Tab opens the HUD." << std::endl;
        return application.exec();
    } catch (const std::exception& error) {
        std::cerr << "EndfieldHUD: " << error.what() << std::endl;
        return 1;
    }
}

#include "main.moc"
