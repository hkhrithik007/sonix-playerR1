#!/usr/bin/env python3
"""Local-only API fixtures for delegated search; never logs headers or tokens."""
import json
import sys
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlsplit


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *_):
        pass

    def do_GET(self):
        path = urlsplit(self.path).path
        item = {"id": 123, "title": "Tidal Track", "duration": 180,
                "artist": {"id": 456, "name": "Tidal Artist"},
                "album": {"id": 789, "title": "Tidal Album"}}
        if path.startswith('/tidal/'):
            if '/search/albums' in path:
                item = {"id": 789, "title": "Tidal Album", "artist": item['artist']}
            elif '/search/artists' in path:
                item = {"id": 456, "name": "Tidal Artist"}
            result = {"items": [item], "totalNumberOfItems": 1}
        elif path.startswith('/qobuz/'):
            item['title'] = 'Qobuz Track'
            item['artist']['name'] = 'Qobuz Artist'
            item['album'] = {"id": "00789", "title": "Qobuz Album"}
            bucket = 'tracks'
            if '/album/search' in path:
                bucket = 'albums'
                item = {"id": "00789", "title": "Qobuz Album", "artist": item['artist']}
            elif '/artist/search' in path:
                bucket = 'artists'
                item = {"id": 456, "name": "Qobuz Artist"}
            result = {bucket: {"items": [item], "total": 1}}
        elif path.startswith('/subsonic/'):
            result = {"subsonic-response": {"status": "ok", "version": "1.16.1",
                      "searchResult3": {
                          "song": [{"id": "song123", "title": "Server Track",
                                    "artist": "Server Artist", "albumId": "album789",
                                    "album": "Server Album", "suffix": "flac", "duration": 180}],
                          "album": [{"id": "album789", "name": "Server Album", "artist": "Server Artist"}],
                          "artist": [{"id": "artist456", "name": "Server Artist"}]}}}
        elif path.startswith('/podcast/'):
            result = {"status": "true", "feeds": [{"id": 12345678901,
                      "title": "Podcast Show", "author": "Podcast Author",
                      "url": "http://127.0.0.1/feed.xml", "episodeCount": 1}], "items": []}
        else:
            self.send_error(404)
            return
        data = json.dumps(result).encode()
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(data)))
        self.end_headers()
        self.wfile.write(data)


if __name__ == '__main__':
    ThreadingHTTPServer(('127.0.0.1', int(sys.argv[1])), Handler).serve_forever()
