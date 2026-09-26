#!/usr/bin/env python3
"""A stand-in for Cider's REST API, for the tests that cannot reach the real one.

It answers the endpoints Sung uses, as documented at
https://cider.gitbook.io/welcome-to-gitbook/docs/1.client/rpc, with the
response shapes Cider 2 sends: now-playing wraps the Apple Music attributes in
"info", is-playing answers "is_playing", and amapi/run-v3 wraps Apple's own
answer in "data". Playback runs on a real clock, so a song ends when its time
is up, and Cider's autoplay starts a song of its own unless it is turned off.

Usage: cider_fixture.py [--port N] [--token T] [--log FILE]
Prints "PORT <n>" once listening. Every request is appended to the log as a
JSON line, token included, so a test can check what Sung sent.
"""
import argparse
import json
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

# Real artwork addresses from Apple's CDN (the Innerlight EP and its Barry
# Can't Swim remix, from Apple's public search), as templates the way the
# Apple Music API sends them. Where the network is closed they do not load.
ART = "https://is1-ssl.mzstatic.com/image/thumb/Music115/v4/e0/55/6c/e0556cdc-2162-2a4a-cd10-5715f4e8eed7/190296481123.jpg/{w}x{h}bb.jpg"
REMIX_ART = "https://is1-ssl.mzstatic.com/image/thumb/Music126/v4/13/7a/18/137a181a-3b77-73f9-8f5e-5049ab0359fe/190296336829.jpg/{w}x{h}bb.jpg"


def song(catalog, name, album, seconds, number, library=None, artist="Elderbrook", art=None):
    attributes = {
        "name": name, "artistName": artist, "albumName": album,
        "durationInMillis": seconds * 1000, "trackNumber": number, "discNumber": 1,
        "releaseDate": "2016-07-15", "artwork": {"url": art or ART, "width": 3000, "height": 3000},
        "playParams": {"id": library or catalog, "kind": "song"},
    }
    if library:
        attributes["playParams"].update({"isLibrary": True, "catalogId": catalog})
    return {"id": library or catalog, "type": "library-songs" if library else "songs", "attributes": attributes}


ALBUM_ONE = [
    song("1740000001", "Innerlight", "Innerlight", 214, 1, "i.aaa1"),
    song("1740000002", "Talking", "Innerlight", 188, 2, "i.aaa2"),
    song("1740000003", "Numb", "Innerlight", 201, 3, "i.aaa3"),
]
ALBUM_TWO = [
    song("1650000001", "Inner Light (Barry Can't Swim Remix)", "Inner Light (Remixes)", 305, 1, "i.bbb1", art=REMIX_ART),
    song("1650000002", "Capricorn", "Inner Light (Remixes)", 240, 2, "i.bbb2", art=REMIX_ART),
]
# A library song Apple no longer carries: no playParams, so nothing to play.
GONE = {"id": "i.gone", "type": "library-songs", "attributes": {"name": "Withdrawn", "artistName": "Elderbrook", "albumName": "Innerlight", "durationInMillis": 180000}}
CHART = [song("1500000001", "Chart One", "Charts", 190, 1, artist="Various"), song("1500000002", "Chart Two", "Charts", 200, 2, artist="Various")]
AUTOPLAY = song("1999999999", "Cider's Own Pick", "Autoplay", 230, 1, artist="Someone Else")

def album(identifier, name, tracks, library=True, art=ART):
    return {"id": identifier, "type": "library-albums" if library else "albums",
            "attributes": {"name": name, "artistName": "Elderbrook", "trackCount": len(tracks),
                           "releaseDate": "2016-07-15", "artwork": {"url": art}}}

ALBUMS = {"l.album1": (album("l.album1", "Innerlight", ALBUM_ONE), ALBUM_ONE + [GONE]),
          "l.album2": (album("l.album2", "Inner Light (Remixes)", ALBUM_TWO, art=REMIX_ART), ALBUM_TWO),
          "1740000000": (album("1740000000", "Innerlight", ALBUM_ONE, False), ALBUM_ONE)}
PLAYLIST = {"id": "p.mix", "type": "library-playlists", "attributes": {"name": "Late Night", "canEdit": True, "artwork": {"url": ART}}}
PLAYLIST_SONGS = [ALBUM_TWO[1], ALBUM_ONE[0]]
ARTIST = {"id": "r.elder", "type": "library-artists", "attributes": {"name": "Elderbrook"}}

SONGS = {}
for item in ALBUM_ONE + ALBUM_TWO + CHART + [AUTOPLAY]:
    SONGS[item["id"]] = item
    SONGS[item["attributes"]["playParams"].get("catalogId", item["id"])] = item


class Deck:
    """Cider's player: one song, a clock, and the autoplay setting."""

    def __init__(self):
        self.lock = threading.Lock()
        self.song = None
        self.playing = False
        self.offset = 0.0
        self.started = 0.0
        self.autoplay = True
        self.volume = 0.5

    def elapsed(self):
        if not self.song:
            return 0.0
        total = self.song["attributes"]["durationInMillis"] / 1000
        at = self.offset + (time.monotonic() - self.started if self.playing else 0)
        if at >= total and self.playing:
            # The song ran out: autoplay picks another, otherwise it stops.
            if self.autoplay and self.song is not AUTOPLAY:
                self.load(AUTOPLAY, True)
                return self.elapsed()
            self.playing = False
            self.offset = total
            return total
        return min(at, total)

    def load(self, item, play=True):
        self.song = item
        self.offset = 0.0
        self.started = time.monotonic()
        self.playing = play

    def seek(self, seconds):
        self.elapsed()
        self.offset = max(0.0, seconds)
        self.started = time.monotonic()


deck = Deck()


def listing(items, total, offset, limit, path):
    page = items[offset:offset + limit]
    answer = {"data": page}
    if offset + limit < total:
        base = path.split("?")[0]
        answer["next"] = f"{base}?offset={offset + limit}"
    return answer


def apple(path):
    """What Apple's API answers for one pass-through path."""
    url = urlparse(path)
    query = parse_qs(url.query)
    offset = int(query.get("offset", ["0"])[0])
    limit = int(query.get("limit", ["25"])[0])
    parts = url.path.strip("/").split("/")
    if url.path == "/v1/me/storefront":
        return {"data": [{"id": "gb", "type": "storefronts"}]}
    if url.path == "/v1/me/library/albums":
        albums = [ALBUMS["l.album1"][0], ALBUMS["l.album2"][0]]
        return listing(albums, len(albums), offset, limit, path)
    if url.path == "/v1/me/library/artists":
        return listing([ARTIST], 1, offset, limit, path)
    if url.path == "/v1/me/library/playlists":
        return listing([PLAYLIST], 1, offset, limit, path)
    if parts[:4] == ["v1", "me", "library", "albums"] and len(parts) >= 5:
        found = ALBUMS.get(parts[4])
        if not found:
            return None
        if len(parts) == 6 and parts[5] == "tracks":
            return listing(found[1], len(found[1]), offset, limit, path)
        return {"data": [found[0]]}
    if parts[:4] == ["v1", "me", "library", "playlists"] and len(parts) >= 5 and parts[4] == "p.mix":
        if len(parts) == 6 and parts[5] == "tracks":
            return listing(PLAYLIST_SONGS, len(PLAYLIST_SONGS), offset, limit, path)
        return {"data": [PLAYLIST]}
    if parts[:4] == ["v1", "me", "library", "artists"] and len(parts) == 6 and parts[5] == "albums":
        albums = [ALBUMS["l.album1"][0], ALBUMS["l.album2"][0]]
        return listing(albums, len(albums), offset, limit, path)
    if parts[:3] == ["v1", "catalog", "gb"]:
        if parts[3:] == ["charts"]:
            return {"results": {"songs": [{"chart": "most-played", "data": CHART}]}}
        if parts[3:] == ["search"]:
            term = query.get("term", [""])[0].lower()
            kind = query.get("types", ["songs"])[0]
            pool = {"songs": ALBUM_ONE + ALBUM_TWO + CHART,
                    "albums": [ALBUMS["1740000000"][0]],
                    "artists": [{"id": "1234", "type": "artists", "attributes": {"name": "Elderbrook", "artwork": {"url": ART}}}]}[kind]
            # Apple matches on names, not on field names.
            def named(entry):
                a = entry["attributes"]
                return " ".join(str(a.get(k, "")) for k in ("name", "artistName", "albumName")).lower()
            hits = [i for i in pool if term in named(i)]
            # Catalogue results carry catalogue identifiers.
            hits = [dict(h, id=h["attributes"].get("playParams", {}).get("catalogId", h["id"]), type=kind) for h in hits]
            group = listing(hits, len(hits), offset, limit, path)
            return {"results": {kind: group} if hits else {}}
        if len(parts) >= 5 and parts[3] == "albums" and parts[4] in ALBUMS:
            found = ALBUMS[parts[4]]
            if len(parts) == 6:
                return listing(found[1], len(found[1]), offset, limit, path)
            return {"data": [found[0]]}
    return None


class Handler(BaseHTTPRequestHandler):
    token = ""
    log = None

    def log_message(self, *args):
        pass

    def answer(self, status, body=None):
        data = b"" if body is None else json.dumps(body).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def handle_any(self, method):
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length) if length else b""
        try:
            body = json.loads(raw) if raw else {}
        except ValueError:
            body = {}
        path = self.path
        if Handler.log:
            with open(Handler.log, "a") as out:
                out.write(json.dumps({"method": method, "path": path, "body": body,
                                      "token": self.headers.get("apptoken")}) + "\n")
        # The test's own controls, which Cider does not have.
        if path == "/fixture/external":
            with deck.lock:
                deck.load(CHART[0], True)
            return self.answer(200, {"status": "ok"})
        if path == "/fixture/state":
            with deck.lock:
                at = deck.elapsed()
                return self.answer(200, {"song": deck.song["id"] if deck.song else None,
                                         "playing": deck.playing, "position": at,
                                         "autoplay": deck.autoplay, "volume": deck.volume})
        if Handler.token and self.headers.get("apptoken") != Handler.token:
            return self.answer(403, {"error": "Unauthorized"})
        with deck.lock:
            at = deck.elapsed()
            p = "/api/v1/playback/"
            if method == "GET" and path == p + "active":
                return self.answer(204)
            if method == "GET" and path == p + "is-playing":
                return self.answer(200, {"status": "ok", "is_playing": deck.playing})
            if method == "GET" and path == p + "now-playing":
                if not deck.song:
                    return self.answer(404, {"error": "Nothing is playing"})
                info = dict(deck.song["attributes"])
                info.update({"currentPlaybackTime": at,
                             "remainingTime": info["durationInMillis"] / 1000 - at,
                             "shuffleMode": 0, "repeatMode": 0, "inLibrary": True})
                return self.answer(200, {"status": "ok", "info": info})
            if method == "GET" and path == p + "autoplay":
                return self.answer(200, {"status": "ok", "value": deck.autoplay})
            if method == "GET" and path == p + "volume":
                return self.answer(200, {"status": "ok", "volume": deck.volume})
            if method == "POST":
                if path == p + "play-item" and body.get("type") == "songs" and body.get("id") in SONGS:
                    deck.load(SONGS[body["id"]])
                    return self.answer(200, {"status": "ok"})
                if path == p + "play-item-href":
                    item = SONGS.get(str(body.get("href", "")).rsplit("/", 1)[-1])
                    if not item:
                        return self.answer(404, {"error": "Not found"})
                    deck.load(item)
                    return self.answer(200, {"status": "ok"})
                if path == p + "play":
                    if deck.song and not deck.playing:
                        deck.offset, deck.started, deck.playing = at, time.monotonic(), True
                    return self.answer(200, {"status": "ok"})
                if path == p + "pause":
                    deck.offset, deck.playing = at, False
                    return self.answer(200, {"status": "ok"})
                if path == p + "seek":
                    deck.seek(float(body.get("position", 0)))
                    return self.answer(204)
                if path == p + "volume":
                    deck.volume = float(body.get("volume", deck.volume))
                    return self.answer(200, {"status": "ok"})
                if path == p + "toggle-autoplay":
                    deck.autoplay = not deck.autoplay
                    return self.answer(200, {"status": "ok"})
                if path == "/api/v1/amapi/run-v3":
                    found = apple(str(body.get("path", "")))
                    if found is None:
                        return self.answer(404, {"error": "Not found"})
                    return self.answer(200, {"data": found})
        return self.answer(404, {"error": "Unknown endpoint"})

    def do_GET(self):
        self.handle_any("GET")

    def do_POST(self):
        self.handle_any("POST")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=0)
    parser.add_argument("--token", default="")
    parser.add_argument("--log")
    args = parser.parse_args()
    Handler.token = args.token
    Handler.log = args.log
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print("PORT", server.server_address[1], flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    sys.exit(main())
