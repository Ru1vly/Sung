// Apple Music through Cider, against tests/cider_fixture.py: a stand-in that
// answers Cider's documented REST API with Cider's response shapes and plays
// on a real clock. The real Cider needs an Apple Music subscription and a
// desktop, so it is checked by hand; see the README.
#include "backend.h"
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>
#include <functional>

class CiderTest : public QObject {
  Q_OBJECT
  QTemporaryDir storage;
  QProcess fixture;
  QString address, log;
  Backend *b = nullptr;
  QNetworkAccessManager network;

  QVariantMap state() {
    QNetworkRequest r(QUrl(address + "/fixture/state"));
    auto *reply = network.get(r);
    QSignalSpy done(reply, &QNetworkReply::finished);
    if (!done.wait(5000))
      return {};
    reply->deleteLater();
    return QJsonDocument::fromJson(reply->readAll()).object().toVariantMap();
  }
  void external() {
    auto *reply = network.post(QNetworkRequest(QUrl(address + "/fixture/external")), QByteArray());
    QSignalSpy done(reply, &QNetworkReply::finished);
    done.wait(5000);
    reply->deleteLater();
  }
  QList<QVariantMap> requests() {
    QList<QVariantMap> out;
    QFile f(log);
    if (f.open(QIODevice::ReadOnly))
      for (const auto &line : f.readAll().split('\n'))
        if (!line.trimmed().isEmpty())
          out << QJsonDocument::fromJson(line).object().toVariantMap();
    return out;
  }
  int count(const QString &path) {
    int n = 0;
    for (const auto &r : requests())
      n += r.value("path").toString() == path;
    return n;
  }
  Cider *cider() { return b->server()->cider(); }
  QVariantMap browse(const QVariantMap &request) {
    QVariantMap result;
    QString failure;
    bool done = false;
    cider()->browse(request, [&](const QVariantMap &d, const QString &e) {
      result = d;
      failure = e;
      done = true;
    });
    [&] { QTRY_VERIFY_WITH_TIMEOUT(done, 10000); }();
    if (!failure.isEmpty())
      result["error"] = failure;
    return result;
  }
  void connectWith(const QString &token) {
    b->server()->connectServer(address, {}, token, false);
    QTRY_VERIFY_WITH_TIMEOUT(!b->server()->connecting(), 10000);
  }

private slots:
  void initTestCase() {
    qputenv("XDG_CONFIG_HOME", (storage.path() + "/config").toUtf8());
    qputenv("XDG_DATA_HOME", (storage.path() + "/data").toUtf8());
    qputenv("XDG_CACHE_HOME", (storage.path() + "/cache").toUtf8());
    QCoreApplication::setOrganizationName("SungTests");
    QCoreApplication::setApplicationName("sung-cider-test");
    log = storage.path() + "/requests.jsonl";
    fixture.start("python3", {qEnvironmentVariable("SUNG_CIDER_FIXTURE"), "--token",
                              "fixture-token", "--log", log});
    QVERIFY(fixture.waitForReadyRead(10000));
    const auto line = QString::fromUtf8(fixture.readLine()).trimmed();
    QVERIFY2(line.startsWith("PORT "), qPrintable(line));
    address = "http://127.0.0.1:" + line.mid(5);
    b = new Backend;
    b->setVolume(0.4);
    b->setAutoplay(false);
    b->setPrepareNext(false);
    b->server()->selectProvider("cider");
    QVERIFY(b->server()->remotePlayback());
    QCOMPARE(b->server()->modes(), (QStringList{"albums", "artists", "playlists", "random"}));
  }

  void refusesAWrongToken() {
    connectWith("not-the-token");
    QVERIFY(!b->server()->connected());
    QVERIFY2(b->server()->error().contains("did not accept the token"),
             qPrintable(b->server()->error()));
  }

  void saysWhenCiderIsNotRunning() {
    b->server()->connectServer("http://127.0.0.1:9", {}, "x", false);
    QTRY_VERIFY_WITH_TIMEOUT(!b->server()->connecting(), 10000);
    QVERIFY(!b->server()->connected());
    QVERIFY2(b->server()->error().contains("Could not reach Cider"),
             qPrintable(b->server()->error()));
  }

  void connectsWithTheBareToken() {
    connectWith("fixture-token");
    QVERIFY2(b->server()->connected(), qPrintable(b->server()->error()));
    QVERIFY(b->server()->error().isEmpty());
    QVERIFY(!b->server()->identity().isEmpty());
    // Cider reads the token alone from its own header, never as a bearer.
    const auto sent = requests();
    QVERIFY(!sent.isEmpty());
    QCOMPARE(sent.last().value("token").toString(), QString("fixture-token"));
    QCOMPARE(sent.last().value("body").toMap().value("path").toString(),
             QString("/v1/me/storefront"));
  }

  void listsTheLibrary() {
    const auto albums = browse({{"mode", "albums"}});
    QVERIFY2(!albums.contains("error"), qPrintable(albums.value("error").toString()));
    const auto rows = albums.value("items").toList();
    QCOMPARE(rows.size(), 2);
    const auto first = rows[0].toMap();
    QCOMPARE(first.value("kind").toString(), QString("album"));
    QCOMPARE(first.value("title").toString(), QString("Innerlight"));
    QCOMPARE(first.value("source").toString(), QString("cider"));
    QCOMPARE(first.value("remoteId").toString(), QString("l.album1"));
    // Apple's artwork template is filled in at the size the interface uses.
    QVERIFY(first.value("art").toString().endsWith("/600x600bb.jpg"));
    QVERIFY(!first.value("art").toString().contains('{'));
    QCOMPARE(browse({{"mode", "artists"}}).value("items").toList().size(), 1);
    QCOMPARE(browse({{"mode", "playlists"}}).value("items").toList().size(), 1);
    QVERIFY(browse({{"mode", "genres"}}).contains("error"));
  }

  void opensAnAlbum() {
    const auto album = browse({{"mode", "album"}, {"remoteId", "l.album1"}});
    QCOMPARE(album.value("title").toString(), QString("Innerlight"));
    QCOMPARE(album.value("artist").toString(), QString("Elderbrook"));
    QCOMPARE(album.value("year").toInt(), 2016);
    const auto songs = album.value("items").toList();
    QCOMPARE(songs.size(), 4);
    const auto song = songs[0].toMap();
    QCOMPARE(song.value("kind").toString(), QString("song"));
    QCOMPARE(song.value("seconds").toInt(), 214);
    QCOMPARE(song.value("catalogId").toString(), QString("1740000001"));
    QCOMPARE(song.value("remoteType").toString(), QString("library-songs"));
    QVERIFY(song.value("available").toBool());
    // A library song Apple no longer carries cannot be offered for play.
    QVERIFY(!songs[3].toMap().value("available").toBool());
    int available = 0;
    for (const auto &v : songs)
      available += v.toMap().value("available").toBool();
    QCOMPARE(available, 3);
    const auto playlist = browse({{"mode", "playlist"}, {"remoteId", "p.mix"}});
    QCOMPARE(playlist.value("title").toString(), QString("Late Night"));
    QCOMPARE(playlist.value("items").toList().size(), 2);
  }

  void searchesTheCatalogueInTheListenersCountry() {
    const auto found = browse({{"mode", "search"}, {"query", "numb"}, {"filter", "songs"}});
    const auto rows = found.value("items").toList();
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows[0].toMap().value("title").toString(), QString("Numb"));
    QCOMPARE(rows[0].toMap().value("catalogId").toString(), QString("1740000003"));
    bool storefront = false;
    for (const auto &r : requests())
      storefront |= r.value("body").toMap().value("path").toString().startsWith("/v1/catalog/gb/search?term=numb");
    QVERIFY(storefront);
    QCOMPARE(browse({{"mode", "search"}, {"query", "inner"}, {"filter", "albums"}})
                 .value("items").toList().value(0).toMap().value("kind").toString(),
             QString("album"));
    QCOMPARE(browse({{"mode", "random"}}).value("items").toList().size(), 2);
  }

  // The whole round: Sung's queue, Cider's sound, and Sung moving on when
  // Cider's clock says the song is over.
  void playsTheQueueThroughCider() {
    const auto album = browse({{"mode", "album"}, {"remoteId", "l.album1"}});
    b->results()->assign(album.value("items").toList());
    b->playResults(0);
    QCOMPARE(b->current().value("title").toString(), QString("Innerlight"));
    QVERIFY(b->externalPlayback());
    QTRY_VERIFY_WITH_TIMEOUT(b->playing(), 10000);
    auto now = state();
    QCOMPARE(now.value("song").toString(), QString("i.aaa1"));
    QVERIFY(now.value("playing").toBool());
    // Sung's volume is Cider's volume while Sung drives.
    QTRY_COMPARE_WITH_TIMEOUT(state().value("volume").toDouble(), 0.4, 5000);
    // Cider's own autoplay is held off so the queue stays Sung's.
    QTRY_VERIFY_WITH_TIMEOUT(!state().value("autoplay").toBool(), 5000);
    QCOMPARE(b->duration(), qint64(214000));
    const auto before = b->position();
    QTest::qWait(1200);
    QVERIFY(b->position() > before);

    b->pause();
    QTRY_VERIFY_WITH_TIMEOUT(!state().value("playing").toBool(), 5000);
    QVERIFY(!b->playing());
    b->play();
    QTRY_VERIFY_WITH_TIMEOUT(state().value("playing").toBool(), 5000);

    // Near the end, then over it: the next song in Sung's queue follows.
    b->seek(211500);
    QTRY_VERIFY_WITH_TIMEOUT(state().value("position").toDouble() > 211, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(b->current().value("title").toString(), QString("Talking"), 12000);
    QTRY_COMPARE_WITH_TIMEOUT(state().value("song").toString(), QString("i.aaa2"), 8000);
    QTRY_VERIFY_WITH_TIMEOUT(b->playing(), 8000);
    QVERIFY(count("/api/v1/playback/play-item") >= 2);
  }

  void control(const QString &path, const QJsonObject &body = {}) {
    QNetworkRequest r(QUrl(address + path));
    r.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    auto *reply = network.post(r, QJsonDocument(body).toJson());
    QSignalSpy done(reply, &QNetworkReply::finished);
    done.wait(5000);
    reply->deleteLater();
  }
  // Holds for the whole stretch, sampled every 100 ms.
  bool throughout(const std::function<bool()> &predicate, int milliseconds) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < milliseconds) {
      if (!predicate())
        return false;
      QTest::qWait(100);
    }
    return true;
  }

  // Apple's player takes a moment to act, and reports the old state until it
  // has. The button must show what was asked, not flick back to what Cider
  // said a moment before.
  void readsThroughCidersDelay() {
    b->playItem(browse({{"mode", "album"}, {"remoteId", "l.album1"}}).value("items").toList()[1].toMap());
    QTRY_VERIFY_WITH_TIMEOUT(b->playing(), 8000);
    control("/fixture/lag", {{"seconds", 1.5}});
    b->pause();
    QVERIFY(!b->playing());
    QVERIFY(throughout([&] { return !b->playing(); }, 2600));
    QTRY_VERIFY_WITH_TIMEOUT(!state().value("playing").toBool(), 3000);
    b->play();
    QVERIFY(b->playing());
    QVERIFY(throughout([&] { return b->playing(); }, 2600));
    QTRY_VERIFY_WITH_TIMEOUT(state().value("playing").toBool(), 3000);
    // Pressed quickly several times, it ends where the last press left it,
    // on both sides.
    b->pause(); QTest::qWait(150); b->play(); QTest::qWait(150); b->pause();
    QVERIFY(throughout([&] { return !b->playing(); }, 2600));
    QTRY_VERIFY_WITH_TIMEOUT(!state().value("playing").toBool(), 4000);
    QVERIFY(throughout([&] { return !b->playing() && !state().value("playing").toBool(); }, 2500));
    control("/fixture/lag", {{"seconds", 0}});
    b->play();
    QTRY_VERIFY_WITH_TIMEOUT(state().value("playing").toBool(), 3000);
  }

  // Play and pause pressed in Cider's own window, or its media keys, show in
  // Sung while Sung is on screen.
  void followsPlayAndPausePressedInCider() {
    QTRY_VERIFY_WITH_TIMEOUT(b->playing(), 5000);
    QTest::qWait(3200);
    control("/fixture/pause");
    QTRY_VERIFY_WITH_TIMEOUT(!b->playing(), 3000);
    control("/fixture/resume");
    QTRY_VERIFY_WITH_TIMEOUT(b->playing(), 4000);
    // Hidden and paused, Cider is left alone.
    b->pause();
    b->setUiActive(false);
    QTest::qWait(400);
    const auto before = count("/api/v1/playback/now-playing");
    QTest::qWait(4500);
    QCOMPARE(count("/api/v1/playback/now-playing"), before);
    b->setUiActive(true);
    b->play();
    QTRY_VERIFY_WITH_TIMEOUT(b->playing() && state().value("playing").toBool(), 5000);
  }

  void songsOnlyInTheLibraryPlayByAddress() {
    auto song = browse({{"mode", "album"}, {"remoteId", "l.album1"}}).value("items").toList()[2].toMap();
    song.remove("catalogId");
    b->playItem(song);
    QTRY_COMPARE_WITH_TIMEOUT(state().value("song").toString(), QString("i.aaa3"), 8000);
    bool href = false;
    for (const auto &r : requests())
      href |= r.value("path") == "/api/v1/playback/play-item-href" &&
              r.value("body").toMap().value("href") == "/v1/me/library/songs/i.aaa3";
    QVERIFY(href);
  }

  void stepsBackWhenTheListenerTakesOverInCider() {
    QTRY_VERIFY_WITH_TIMEOUT(b->playing(), 8000);
    QSignalSpy failed(cider(), &Cider::failed);
    external();
    QTRY_VERIFY_WITH_TIMEOUT(!failed.isEmpty(), 5000);
    QVERIFY(failed.first().first().toString().contains("something else"));
    QVERIFY(!b->playing());
    QVERIFY(!cider()->active());
    // Cider's autoplay goes back to how the listener had it, and after that
    // nothing more is sent while the listener is in charge.
    QTRY_VERIFY_WITH_TIMEOUT(state().value("autoplay").toBool(), 5000);
    QTest::qWait(300);
    const auto sent = requests().size();
    QTest::qWait(1500);
    QCOMPARE(requests().size(), sent);
  }

  void handsBackToSungsOwnDecksAndRestoresAutoplay() {
    b->playItem(browse({{"mode", "album"}, {"remoteId", "l.album2"}}).value("items").toList()[0].toMap());
    QTRY_VERIFY_WITH_TIMEOUT(b->playing(), 8000);
    b->stop();
    QVERIFY(!b->externalPlayback());
    QTRY_VERIFY_WITH_TIMEOUT(!state().value("playing").toBool(), 5000);
    QTRY_VERIFY_WITH_TIMEOUT(state().value("autoplay").toBool(), 5000);
  }

  // Run from aboutToQuit, after which nothing more would be sent.
  void quittingPausesCiderAndGivesAutoplayBack() {
    b->playItem(browse({{"mode", "album"}, {"remoteId", "l.album1"}}).value("items").toList()[0].toMap());
    QTRY_VERIFY_WITH_TIMEOUT(b->playing(), 8000);
    QTRY_VERIFY_WITH_TIMEOUT(!state().value("autoplay").toBool(), 5000);
    QElapsedTimer took;
    took.start();
    cider()->shutDown();
    QVERIFY(took.elapsed() < 1000);
    const auto after = state();
    QVERIFY(!after.value("playing").toBool());
    QVERIFY(after.value("autoplay").toBool());
    b->stop();
  }

  void disconnectingStopsFollowing() {
    b->server()->disconnectServer();
    QVERIFY(!b->server()->connected());
    const auto song = browse({{"mode", "albums"}});
    QVERIFY(song.contains("error"));
  }

  // The real Cider, when a token is given: SUNG_CIDER_LIVE_TOKEN, and
  // SUNG_CIDER_LIVE_ADDRESS if Cider is not on http://localhost:10767. It
  // searches Apple's catalogue, plays the first song for a few seconds,
  // pauses it and lets go, so run it with nothing playing in Cider.
  void againstTheRealCider() {
    const auto token = qEnvironmentVariable("SUNG_CIDER_LIVE_TOKEN");
    if (token.isEmpty())
      QSKIP("Set SUNG_CIDER_LIVE_TOKEN to check against a running Cider.");
    Cider live(false);
    live.connectServer(qEnvironmentVariable("SUNG_CIDER_LIVE_ADDRESS"), {}, token, false);
    QTRY_VERIFY_WITH_TIMEOUT(!live.connecting(), 20000);
    QVERIFY2(live.connected(), qPrintable(live.error()));
    QVariantMap found;
    QString failure;
    bool done = false;
    live.browse({{"mode", "search"}, {"query", "Elderbrook"}, {"filter", "songs"}},
                [&](const QVariantMap &d, const QString &e) { found = d; failure = e; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 20000);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    const auto rows = found.value("items").toList();
    QVERIFY(!rows.isEmpty());
    const auto song = rows.first().toMap();
    QVERIFY(song.value("seconds").toInt() > 0);
    QVERIFY(song.value("art").toString().startsWith("https://"));
    done = false;
    live.browse({{"mode", "albums"}}, [&](const QVariantMap &, const QString &e) { failure = e; done = true; });
    QTRY_VERIFY_WITH_TIMEOUT(done, 20000);
    QVERIFY2(failure.isEmpty(), qPrintable(failure));
    QSignalSpy failed(&live, &Cider::failed);
    live.play(song, 0);
    QTRY_VERIFY_WITH_TIMEOUT(live.playing() || !failed.isEmpty(), 20000);
    QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed.first().first().toString()));
    const auto start = live.position();
    QTest::qWait(3000);
    QVERIFY(live.position() > start + 1500);
    live.pause();
    QVERIFY(!live.playing());
    live.release();
    live.disconnectServer();
  }

  void cleanupTestCase() {
    delete b;
    fixture.kill();
    fixture.waitForFinished(3000);
  }
};
QTEST_MAIN(CiderTest)
#include "cider_test.moc"
