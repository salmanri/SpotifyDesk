# -*- coding: utf-8 -*-
"""
SpotifyDesk V2 — Spotify OAuth2 Token Generator
Script ini membantu Anda mendapatkan Refresh Token Spotify dengan 1 kali klik.
"""
import os
import sys
import webbrowser
import urllib.parse
import urllib.request
import json
import base64
from http.server import HTTPServer, BaseHTTPRequestHandler

REDIRECT_URI = "http://127.0.0.1:8888/callback"
AUTH_CODE = None

class CallbackHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        global AUTH_CODE
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/callback":
            qs = urllib.parse.parse_qs(parsed.query)
            if "code" in qs:
                AUTH_CODE = qs["code"][0]
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.end_headers()
                html = """
                <html>
                <body style="font-family: Arial, sans-serif; text-align: center; padding-top: 50px;">
                    <h2 style="color: #1DB954;">&#10004; Otorisasi Spotify Berhasil!</h2>
                    <p>Token telah berhasil didapatkan. Anda boleh menutup tab ini dan kembali ke terminal.</p>
                </body>
                </html>
                """
                self.wfile.write(html.encode("utf-8"))
            else:
                self.send_response(400)
                self.end_headers()
                self.wfile.write(b"Error: Code parameter missing.")
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, format, *args):
        pass

def main():
    print("=" * 60)
    print("  SpotifyDesk V2 — Spotify OAuth2 Token Generator")
    print("=" * 60)
    print()

    config_path = os.path.join(os.path.dirname(__file__), "SpotifyDeskV2", "config.h")
    client_id = ""
    client_secret = ""

    if os.path.exists(config_path):
        with open(config_path, "r", encoding="utf-8") as f:
            for line in f:
                if "#define SPOTIFY_CLIENT_ID" in line and 'MASUKKAN' not in line:
                    parts = line.split('"')
                    if len(parts) >= 2: client_id = parts[1]
                if "#define SPOTIFY_CLIENT_SECRET" in line and 'MASUKKAN' not in line:
                    parts = line.split('"')
                    if len(parts) >= 2: client_secret = parts[1]

    if not client_id or client_id.startswith("MASUKKAN"):
        client_id = input("Masukkan Spotify Client ID : ").strip()
    else:
        print(f"Spotify Client ID terdeteksi dari config.h: {client_id[:8]}...")

    if not client_secret or client_secret.startswith("MASUKKAN"):
        client_secret = input("Masukkan Spotify Client Secret : ").strip()
    else:
        print(f"Spotify Client Secret terdeteksi dari config.h: {client_secret[:8]}...")

    if not client_id or not client_secret:
        print("[ERROR] Client ID dan Client Secret wajib diisi!")
        sys.exit(1)

    scope = "user-read-playback-state user-read-currently-playing"
    auth_params = {
        "client_id": client_id,
        "response_type": "code",
        "redirect_uri": REDIRECT_URI,
        "scope": scope,
    }
    auth_url = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode(auth_params)

    print()
    print("Membuka browser untuk otorisasi Spotify...")
    print("Jika browser tidak terbuka otomatis, buka link ini:")
    print(auth_url)
    print()
    webbrowser.open(auth_url)

    server = HTTPServer(("localhost", 8888), CallbackHandler)
    print("Menunggu konfirmasi login dari browser...")
    while AUTH_CODE is None:
        server.handle_request()

    print("[OK] Kode otorisasi diterima. Menukarkan dengan Refresh Token...")

    token_url = "https://accounts.spotify.com/api/token"
    data = urllib.parse.urlencode({
        "grant_type": "authorization_code",
        "code": AUTH_CODE,
        "redirect_uri": REDIRECT_URI,
    }).encode("utf-8")

    cred = f"{client_id}:{client_secret}".encode("utf-8")
    b64_cred = base64.b64encode(cred).decode("utf-8")

    req = urllib.request.Request(token_url, data=data)
    req.add_header("Authorization", f"Basic {b64_cred}")
    req.add_header("Content-Type", "application/x-www-form-urlencoded")

    try:
        with urllib.request.urlopen(req) as response:
            res_body = response.read().decode("utf-8")
            token_json = json.loads(res_body)
            refresh_token = token_json.get("refresh_token")

            if not refresh_token:
                print("[ERROR] Refresh Token tidak ditemukan di respons:")
                print(res_body)
                sys.exit(1)

            print()
            print("=" * 60)
            print("  SUKSES! Refresh Token Berhasil Didapatkan:")
            print("=" * 60)
            print(refresh_token)
            print("=" * 60)

            if os.path.exists(config_path):
                with open(config_path, "r", encoding="utf-8") as f:
                    content = f.read()

                import re
                content = re.sub(r'#define SPOTIFY_CLIENT_ID\s+".*?"', f'#define SPOTIFY_CLIENT_ID     "{client_id}"', content)
                content = re.sub(r'#define SPOTIFY_CLIENT_SECRET\s+".*?"', f'#define SPOTIFY_CLIENT_SECRET "{client_secret}"', content)
                content = re.sub(r'#define SPOTIFY_REFRESH_TOKEN\s+".*?"', f'#define SPOTIFY_REFRESH_TOKEN "{refresh_token}"', content)

                with open(config_path, "w", encoding="utf-8") as f:
                    f.write(content)

                print()
                print("[OK] File SpotifyDeskV2/config.h telah otomatis diperbarui!")
                print("Sekarang Anda hanya perlu mengisi WIFI_SSID dan WIFI_PASSWORD di config.h,")
                print("lalu upload sketch SpotifyDeskV2 ke ESP32!")
    except urllib.error.HTTPError as e:
        print(f"[ERROR] HTTP Error {e.code}: {e.read().decode('utf-8')}")
    except Exception as e:
        print(f"[ERROR] {e}")

if __name__ == "__main__":
    main()
