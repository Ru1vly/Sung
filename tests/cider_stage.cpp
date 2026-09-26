#include "uitest.h"
#include "backend.h"
#include <QAccessible>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QProcess>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSignalSpy>
#include <QtTest/QTest>
#include <ctime>
#include <functional>
#include <qpa/qwindowsysteminterface.h>

// Apple Music through Cider, driven the way a person drives it: the Music
// server tab, the Cider choice in the connection dialog, the token typed in,
// an album opened and a song clicked, the play button, a click far along the
// seek bar, and Sung moving to the next song when Cider's clock runs out.
// tests/cider_fixture.py stands in for Cider, which needs a desktop and an
// Apple Music subscription; it answers Cider's documented API on a real clock.
namespace {
QQuickItem *shown(QQuickItem *root, const QString &name) {
  if (!root || !root->isVisible()) return nullptr;
  if (root->objectName() == name) return root;
  for (auto *child : root->childItems())
    if (auto *found = shown(child, name)) return found;
  return nullptr;
}
// A visible control by the words on it, for the few without an object name.
QQuickItem *labelled(QQuickItem *root, const QString &text) {
  if (!root || !root->isVisible()) return nullptr;
  if (root->property("text").toString() == text && root->property("checkable").isValid()) return root;
  for (auto *child : root->childItems())
    if (auto *found = labelled(child, text)) return found;
  return nullptr;
}
// QTest types strings only into widgets; a window takes one key at a time.
void keys(QQuickWindow *window, const QString &text) {
  for (const auto ch : text) QTest::keyClick(window, ch.toLatin1());
}
QString accessibleName(QQuickItem *item) {
  auto *iface = item ? QAccessible::queryAccessibleInterface(item) : nullptr;
  return iface ? iface->text(QAccessible::Name) : QString();
}
struct Stage {
  Backend *backend;
  QQuickWindow *window;
  QString directory;
  int failures = 0;
  void check(bool ok, const QString &label) {
    fprintf(stdout, "%s %s\n", ok ? "PASS" : "FAIL", qPrintable(label));
    fflush(stdout);
    if (!ok) ++failures;
  }
  bool until(const std::function<bool()> &predicate, int timeout = 8000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeout) QTest::qWait(25);
    return predicate();
  }
  void shot(const QString &name) {
    QTest::qWait(400);
    check(window->grabWindow().save(directory + '/' + name + ".png"), "capture " + name);
  }
  QPoint centre(QQuickItem *item) { return item->mapToScene(item->boundingRect().center()).toPoint(); }
  void press(QQuickItem *item, const QString &label) {
    check(item, "find " + label);
    if (!item) return;
    QTest::mouseMove(window, centre(item));
    QTest::qWait(60);
    QTest::mouseClick(window, Qt::LeftButton, Qt::NoModifier, centre(item));
    QTest::qWait(300);
  }
  void click(const QString &name) { press(shown(window->contentItem(), name), name); }
  // Clicks into a field, clears it the way a person would and types.
  void type(const QString &name, const QString &text) {
    click(name);
    QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier);
    QTest::keyClick(window, Qt::Key_Backspace);
    keys(window, text);
    QTest::qWait(100);
  }
};
} // namespace

void runCiderTests(Backend *b, QQuickWindow *w) {
  Stage c{b, w, qEnvironmentVariable("SUNG_TEST_OUTPUT")};
  QDir().mkpath(c.directory);
  const auto finish = [&](QProcess *fixture) {
    if (fixture) { fixture->kill(); fixture->waitForFinished(3000); }
    fprintf(stdout, "RESULT %d failures\n", c.failures);
    QCoreApplication::exit(c.failures ? 1 : 0);
  };
  auto *fixture = new QProcess(qApp);
  const auto log = c.directory + "/cider-requests.jsonl";
  QFile::remove(log);
  QStringList fixtureArguments{qEnvironmentVariable("SUNG_CIDER_FIXTURE"), "--token", "stage-token", "--log", log};
  // Heard on the real default output through paplay, for a run against a
  // real sound server and parec. Off by default: nobody wants a test tone.
  if (qEnvironmentVariableIsSet("SUNG_CIDER_AUDIBLE")) fixtureArguments << "--audible";
  fixture->start("python3", fixtureArguments);
  c.check(fixture->waitForReadyRead(10000), "the stand-in Cider starts");
  const auto line = QString::fromUtf8(fixture->readLine()).trimmed();
  if (!line.startsWith("PORT ")) { c.check(false, "the stand-in Cider reports its port"); finish(fixture); return; }
  const QString address = "http://127.0.0.1:" + line.mid(5);
  QNetworkAccessManager network;
  auto fixtureState = [&] {
    auto *reply = network.get(QNetworkRequest(QUrl(address + "/fixture/state")));
    QSignalSpy done(reply, &QNetworkReply::finished);
    done.wait(5000);
    reply->deleteLater();
    return QJsonDocument::fromJson(reply->readAll()).object().toVariantMap();
  };

  QWindowSystemInterface::handleFocusWindowChanged(w);
  w->resize(1180, 800);
  b->setTheme("dark");
  b->setVolume(0.3);
  b->setAutoplay(false);
  b->setPrepareNext(false);
  QTest::qWait(600);

  // The Music server tab, empty until something is connected.
  QQuickItem *library = nullptr;
  for (const auto &name : {"navBar_library", "nav_library", "drawerNav_library"})
    if (!library) library = shown(w->contentItem(), name);
  c.press(library, "the Library destination");
  c.click("serverTab");
  c.check(c.until([&] { return shown(w->contentItem(), "serverEmptyState") != nullptr; }),
          "the Music server tab offers to connect");
  c.shot("01-server-empty");

  c.click("serverEmptyConnect");
  c.check(c.until([&] { return shown(w->contentItem(), "serverProvider") != nullptr; }), "the connection dialog opens");
  QTest::qWait(400);
  c.click("serverType_cider");
  c.check(b->server()->provider() == "cider", "choosing Cider selects it");
  auto *addressField = shown(w->contentItem(), "serverAddress");
  auto *tokenField = shown(w->contentItem(), "serverPassword");
  c.check(addressField && addressField->property("text").toString() == "http://localhost:10767",
          "the address starts at Cider's default");
  c.check(!shown(w->contentItem(), "serverUsername"), "Cider asks for no user name");
  c.check(tokenField && tokenField->property("label").toString() == "Cider token", "the second field is Cider's token");
  c.check(tokenField && tokenField->property("supporting").toString().contains("Manage External Application Access"),
          "the token field says where Cider makes the token");
  c.check(accessibleName(tokenField) == "Cider token", "screen readers hear the token field by name");
  if (auto *remember = shown(w->contentItem(), "rememberServer"); remember && remember->property("checked").toBool())
    c.click("rememberServer");  // Tests never write a real keyring entry.
  c.shot("02-connection-cider");

  // Tab goes straight from the address to the token, past the hidden name.
  c.type("serverAddress", address);
  QTest::keyClick(w, Qt::Key_Tab);
  QTest::qWait(150);
  c.check(tokenField && tokenField->hasActiveFocus(), "Tab moves from the address to the token");

  keys(w, "wrong-token");
  c.click("connectServerButton");
  auto *status = shown(w->contentItem(), "serverStatus");
  c.check(c.until([&] { return !b->server()->connecting() && !b->server()->error().isEmpty(); }),
          "a wrong token is refused");
  c.check(status && status->property("text").toString().contains("did not accept the token"),
          "the dialog says the token was not accepted");
  c.shot("03-wrong-token");

  c.type("serverPassword", "stage-token");
  QTest::keyClick(w, Qt::Key_Return);
  c.check(c.until([&] { return b->server()->connected(); }), "the right token connects");
  c.check(status && status->property("text").toString() == "Connected", "the dialog says Connected");
  c.check(!labelled(w->contentItem(), "Update server listening history"),
          "server listening history is not offered for Cider");
  c.shot("04-connected");
  QTest::keyClick(w, Qt::Key_Escape);
  QTest::qWait(450);

  // Only the views Cider can fill are offered.
  c.check(!labelled(w->contentItem(), "Genres") && !labelled(w->contentItem(), "Favorites"),
          "Genres and Favorites are not offered");
  auto *albums = labelled(w->contentItem(), "Albums");
  c.press(albums, "the Albums chip");
  c.check(c.until([&] { return !b->busy() && b->results()->count() == 2; }), "the Apple Music library's albums list");
  c.shot("05-albums");

  // The server page lists albums as rows; the first opens like any other.
  c.click("trackRow_0");
  c.check(c.until([&] { return !b->busy() && b->results()->count() == 4 && b->title() == "Innerlight"; }),
          "the album opens with its songs");
  c.shot("06-album");

  c.click("trackRow_0");
  c.check(c.until([&] { return b->playing(); }, 10000), "clicking a song plays it");
  c.check(fixtureState().value("song").toString() == "i.aaa1", "Cider plays the song clicked");
  c.check(b->externalPlayback(), "Sung knows the sound is Cider's");
  c.check(c.until([&] { return !fixtureState().value("autoplay").toBool(); }), "Cider's own autoplay is held off");
  c.check(c.until([&] { return qAbs(fixtureState().value("volume").toDouble() - 0.3) < 0.01; }),
          "Sung's volume reaches Cider");
  c.shot("07-playing");

  c.click("playButton");
  c.check(c.until([&] { return !fixtureState().value("playing").toBool(); }), "the play button pauses Cider");
  c.check(!b->playing(), "Sung shows it paused");
  c.click("playButton");
  c.check(c.until([&] { return fixtureState().value("playing").toBool() && b->playing(); }), "and plays it again");

  // What following Cider costs: one reading a second while it plays, none
  // while it is paused, and the process's CPU over the same stretch.
  const auto readings = [&] {
    int n = 0;
    QFile f(log);
    if (f.open(QIODevice::ReadOnly))
      for (const auto &entry : f.readAll().split('\n'))
        n += entry.contains("\"/api/v1/playback/now-playing\"");
    return n;
  };
  const auto cpu = [] {
    timespec now{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &now);
    return now.tv_sec + now.tv_nsec / 1e9;
  };
  QTest::qWait(1500);
  const int before = readings();
  const double cpuBefore = cpu();
  QElapsedTimer span;
  span.start();
  QTest::qWait(6000);
  const double seconds = span.elapsed() / 1000.0;
  const double rate = (readings() - before) / seconds;
  const double load = (cpu() - cpuBefore) / seconds * 100;
  fprintf(stdout, "MEASURE following Cider: %.2f readings/s, %.1f%% of a core\n", rate, load);
  c.check(rate > 0.7 && rate < 1.6, "Cider is read about once a second while it plays");
  c.click("playButton");
  c.check(c.until([&] { return !fixtureState().value("playing").toBool(); }), "paused again");
  QTest::qWait(500);
  const int paused = readings();
  const double pausedCpu = cpu();
  span.restart();
  QTest::qWait(4200);
  const double pausedSeconds = span.elapsed() / 1000.0;
  fprintf(stdout, "MEASURE paused, same view: %.2f readings/s, %.1f%% of a core\n",
          (readings() - paused) / pausedSeconds, (cpu() - pausedCpu) / pausedSeconds * 100);
  // On screen and paused, every two seconds, so a play pressed in Cider shows.
  c.check(readings() - paused >= 1 && readings() - paused <= 3, "read every two seconds while paused on screen");
  c.click("playButton");
  c.check(c.until([&] { return b->playing(); }), "playing once more");

  // Apple's player acts a moment after it is asked. The button shows what
  // was pressed, not what Cider said a moment earlier.
  auto *playButton = shown(w->contentItem(), "playButton");
  const auto symbol = [&] { return playButton ? playButton->property("symbol").toString() : QString(); };
  const auto holds = [&](const QString &expected, int milliseconds) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < milliseconds) {
      if (symbol() != expected) return false;
      QTest::qWait(100);
    }
    return true;
  };
  QTest::qWait(3200);
  QNetworkRequest lagRequest(QUrl(address + "/fixture/lag"));
  lagRequest.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
  auto *lag = network.post(lagRequest, QByteArray("{\"seconds\": 1.5}"));
  QSignalSpy lagged(lag, &QNetworkReply::finished);
  lagged.wait(5000);
  lag->deleteLater();
  c.click("playButton");
  c.check(holds("play", 2600), "a pause stays shown while Cider takes its time");
  c.check(c.until([&] { return !fixtureState().value("playing").toBool(); }), "and Cider pauses");
  c.click("playButton");
  c.check(holds("pause", 2600), "a play stays shown while Cider takes its time");
  c.check(c.until([&] { return fixtureState().value("playing").toBool(); }), "and Cider plays");
  auto *noLag = network.post(lagRequest, QByteArray("{\"seconds\": 0}"));
  QSignalSpy unlagged(noLag, &QNetworkReply::finished);
  unlagged.wait(5000);
  noLag->deleteLater();
  // Minimised, nothing is drawn, so what is left is the following itself.
  w->showMinimized();
  QTest::qWait(1500);
  const int hiddenBefore = readings();
  const double hiddenCpu = cpu();
  span.restart();
  QTest::qWait(6000);
  const double hiddenSeconds = span.elapsed() / 1000.0;
  fprintf(stdout, "MEASURE following Cider minimised: %.2f readings/s, %.1f%% of a core\n",
          (readings() - hiddenBefore) / hiddenSeconds, (cpu() - hiddenCpu) / hiddenSeconds * 100);
  c.check(readings() > hiddenBefore, "Cider is still followed while the window is minimised");
  // Minimised and paused, Cider is left alone; the button cannot be reached,
  // so the pause is the media key's.
  b->pause();
  QTest::qWait(600);
  const int hiddenPaused = readings();
  QTest::qWait(4500);
  c.check(readings() == hiddenPaused, "and not read at all once paused there");
  b->play();
  c.check(c.until([&] { return b->playing(); }), "playing after the minimised pause");
  w->showNormal();
  QWindowSystemInterface::handleFocusWindowChanged(w);
  QTest::qWait(800);

  // A click near the end of the seek bar, then the song runs out.
  auto *seek = shown(w->contentItem(), "seekBar");
  c.check(seek, "find seekBar");
  if (seek) {
    const auto at = seek->mapToScene(QPointF(seek->width() * 0.985, seek->height() / 2)).toPoint();
    QTest::mouseMove(w, at);
    QTest::qWait(80);
    QTest::mouseClick(w, Qt::LeftButton, Qt::NoModifier, at);
  }
  c.check(c.until([&] { return fixtureState().value("position").toDouble() > 200; }), "the seek bar moves Cider");
  c.check(c.until([&] { return b->current().value("title") == "Talking"; }, 15000),
          "Sung moves to the next song when Cider's ends");
  c.check(c.until([&] { return fixtureState().value("song").toString() == "i.aaa2"; }), "Cider plays the next song");
  c.check(c.until([&] { return b->playing(); }), "and it plays");
  c.shot("08-next-song");

  w->setProperty("immersive", true);
  auto *player = c.until([&] { return shown(w->contentItem(), "immersivePlayer") != nullptr; })
      ? shown(w->contentItem(), "immersivePlayer") : nullptr;
  c.check(player, "the immersive view opens on an Apple Music song");
  QTest::qWait(700);
  c.shot("09-immersive");
  c.click("immersiveLayoutButton");
  QTest::qWait(450);
  auto *speed = shown(w->contentItem(), "immersiveSpeed");
  c.check(speed && !speed->isEnabled(), "playback speed is off while Cider plays");
  c.shot("10-immersive-menu");
  // The visualizer hears what the speakers play, since Cider has the sound.
  c.click("immersiveLayout_visualizer");
  const auto loud = [&](const char *property) {
    for (const auto &v : b->property(property).toList()) if (v.toDouble() > 0.05) return true;
    return false;
  };
  c.check(c.until([&] { return loud("audioSpectrum"); }), "the visualizer moves with an Apple Music song");
  c.check(loud("audioLevels"), "and so do the levels the play button and backdrop read");
  QTest::qWait(600);
  c.shot("10b-visualizer");
  c.click("immersivePlayButton");
  c.check(c.until([&] { return !loud("audioLevels") && !loud("audioSpectrum"); }), "paused, the visualizer rests");
  c.click("immersivePlayButton");
  c.check(c.until([&] { return b->playing() && loud("audioSpectrum"); }), "and moves again on play");
  // The layout is kept, so the next captures start from the one they expect.
  c.click("immersiveLayoutButton");
  QTest::qWait(450);
  c.click("immersiveLayout_split");
  QTest::qWait(400);
  w->setProperty("immersive", false);
  QTest::qWait(600);

  // Search goes to Apple Music's catalogue.
  auto *search = shown(w->contentItem(), "searchField");
  c.check(search && search->property("placeholderText").toString() == "Search Apple Music", "the search field names Apple Music");
  if (search) {
    c.press(search, "searchField");
    keys(w, "numb");
    QTest::keyClick(w, Qt::Key_Return);
  }
  c.check(c.until([&] { return !b->busy() && b->results()->count() == 1 && b->results()->get(0).value("title") == "Numb"; }),
          "searching finds the song in the catalogue");
  c.shot("11-search");

  // Narrow and light: the three server types still fit the dialog.
  w->resize(420, 760);
  QTest::qWait(700);
  c.shot("12-narrow");
  QMetaObject::invokeMethod(w, "openServerConnection");
  QTest::qWait(600);
  // Measured against the dialog's own column, which clips what overflows it.
  auto *types = shown(w->contentItem(), "serverProvider");
  auto *fields = shown(w->contentItem(), "connectionFields");
  auto *addressAgain = shown(w->contentItem(), "serverAddress");
  const auto column = fields ? fields->mapRectToScene(fields->boundingRect()) : QRectF();
  const auto within = [&](QQuickItem *item) {
    const auto r = item ? item->mapRectToScene(item->boundingRect()) : QRectF();
    return item && r.left() >= column.left() - 0.5 && r.right() <= column.right() + 0.5;
  };
  c.check(fields && within(types), "the three server types fit a narrow dialog");
  c.check(fields && within(addressAgain), "the fields keep inside a narrow dialog");
  c.shot("13-narrow-connection");
  QTest::keyClick(w, Qt::Key_Escape);
  QTest::qWait(400);
  b->setTheme("light");
  w->resize(1180, 800);
  QTest::qWait(700);
  c.shot("14-light");

  // Someone picks a different song in Cider's own window: Sung steps back.
  auto *reply = network.post(QNetworkRequest(QUrl(address + "/fixture/external")), QByteArray());
  QSignalSpy taken(reply, &QNetworkReply::finished);
  taken.wait(5000);
  reply->deleteLater();
  c.check(c.until([&] { return !b->playing() && b->error().contains("something else"); }),
          "Sung stops following when the listener takes over in Cider");
  auto *errorBar = shown(w->contentItem(), "errorBar");
  c.check(errorBar, "the page says so");
  c.shot("15-taken-over");

  // Disconnecting gives Cider its autoplay back.
  QMetaObject::invokeMethod(w, "openServerConnection");
  QTest::qWait(600);
  QQuickItem *disconnect = nullptr;
  std::function<void(QQuickItem *)> find = [&](QQuickItem *item) {
    if (!item || !item->isVisible() || disconnect) return;
    if (item->property("text").toString() == "Disconnect") { disconnect = item; return; }
    for (auto *child : item->childItems()) find(child);
  };
  find(w->contentItem());
  c.press(disconnect, "Disconnect");
  c.check(!b->server()->connected(), "Disconnect lets go of Cider");
  c.check(c.until([&] { return fixtureState().value("autoplay").toBool(); }), "Cider's autoplay is back as it was");
  QTest::keyClick(w, Qt::Key_Escape);
  finish(fixture);
}
