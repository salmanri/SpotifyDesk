"""
SpotifyDesk - Desktop Companion (Windows)
Membaca lagu dari Spotify via Windows Media Session,
lalu mengirim metadata, cover art, lirik aktif (auto-wrap & center), dan sinkronisasi waktu ke ESP32.

Fitur:
- Cover art 96x96 Floyd-Steinberg dithering (4 chunk aman via Serial)
- Lirik sinkron otomatis dari LRCLIB (hanya baris yang sedang dinyanyikan, auto-wrap & rata tengah)
- Otomatis mengosongkan area lirik saat ada jeda panjang / solo musik
- Penghitungan posisi waktu real-time Windows GSMTC tanpa delay
- Sinkronisasi periodik (SYNC) untuk mencegah time drift
"""

import asyncio
import io
import re
import time
import sys
import json
import urllib.request
import urllib.parse
import urllib.error
from datetime import datetime, timezone
from PIL import Image, ImageEnhance
import serial
import serial.tools.list_ports

from winrt.windows.media.control import (
    GlobalSystemMediaTransportControlsSessionManager as MediaManager,
)
from winrt.windows.storage.streams import DataReader

# Pastikan output konsol Windows mendukung UTF-8 tanpa crash UnicodeEncodeError
if sys.platform.startswith("win"):
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except Exception:
        pass

# ============================================================================
# Config
# ============================================================================
BAUD_RATE = 115200
COVER_SIZE = 96
COVER_BYTES = (COVER_SIZE * COVER_SIZE) // 8  # 1152
ART_CHUNK_SIZE = COVER_BYTES // 4  # 288 bytes per chunk = 576 hex chars

SYNC_INTERVAL_SEC = 5  # kirim SYNC tiap 5 detik
LYRIC_LEAD_MS = 800  # standar optimal (~800ms): kompensasi refresh SSD1680 (~700-750ms) + transmisi USB serial


# ============================================================================
# Text Sanitization
# ============================================================================
def clean_text_for_display(text):
    """Bersihkan karakter khusus agar tampil sempurna di font e-paper."""
    if not text:
        return ""
    # Hapus sisa tag bracket lirik jika ada
    text = re.sub(r"\[\d+:\d+\.?\d*\]", "", text)
    text = text.replace("’", "'").replace("‘", "'")
    text = text.replace("“", '"').replace("”", '"')
    text = text.replace("—", "-").replace("–", "-")
    text = text.replace("…", "...")
    text = text.replace("|", "-").replace("\n", " ").replace("\r", "")
    return text.strip()


def clean_title_for_search(title):
    """Hilangkan teks tambahan seperti (Remastered) agar pencarian lirik lebih akurat."""
    t = re.sub(r"\(.*?\)", "", title)
    t = re.sub(r"\[.*?\]", "", t)
    t = re.sub(r"-\s*(remaster|live|mono|stereo|bonus|radio edit).*", "", t, flags=re.IGNORECASE)
    return t.strip()


# ============================================================================
# Serial / Port Detection
# ============================================================================
def find_esp32_port():
    """Mencari COM port ESP32 secara otomatis."""
    ports = list(serial.tools.list_ports.comports())
    for p in ports:
        desc = (p.description or "").lower()
        if any(kw in desc for kw in ["cp210", "ch340", "usb-serial", "uart"]):
            return p.device
    for p in ports:
        if p.device == "COM3":
            return "COM3"
    if ports:
        return ports[0].device
    return None


def wait_for_ack(ser, expected_prefix, timeout=5.0):
    """Tunggu balasan ACK dari ESP32, return True jika diterima."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        if ser.in_waiting:
            try:
                resp = ser.readline().decode("ascii", errors="replace").strip()
                if resp.startswith(expected_prefix):
                    return True
            except Exception:
                pass
        time.sleep(0.01)
    return False


# ============================================================================
# Cover Art Processing
# ============================================================================
def process_thumbnail_to_hex(image_bytes):
    """Konversi gambar thumbnail ke hex bitmap 96x96 (Floyd-Steinberg dithering)."""
    try:
        img = Image.open(io.BytesIO(image_bytes)).convert("RGB")
        w, h = img.size
        d = min(w, h)
        left, top = (w - d) // 2, (h - d) // 2
        img = img.crop((left, top, left + d, top + d))

        img = img.resize((COVER_SIZE, COVER_SIZE), Image.Resampling.LANCZOS)
        gray = img.convert("L")
        contrast = ImageEnhance.Contrast(gray).enhance(1.25)
        bw = contrast.convert("1", dither=Image.Dither.FLOYDSTEINBERG)

        # Format Adafruit GFX: bit 1 = Hitam, bit 0 = Putih
        data = bytearray()
        for y in range(COVER_SIZE):
            byte_val = 0
            for x in range(COVER_SIZE):
                px = bw.getpixel((x, y))
                if px == 0:  # hitam
                    byte_val |= 1 << (7 - (x % 8))
                if (x % 8) == 7:
                    data.append(byte_val)
                    byte_val = 0
        return data.hex()
    except Exception as e:
        print(f"  [WARN] Gagal proses thumbnail: {e}")
        return ""


def send_art_chunked(ser, art_hex):
    """Kirim bitmap cover art ke ESP32 dalam 4 chunk terpisah."""
    if not art_hex or len(art_hex) < COVER_BYTES * 2:
        ser.write(b"ART_END\n")
        ser.flush()
        wait_for_ack(ser, "ACK_DRAW", timeout=10.0)
        return

    chunk_hex_size = ART_CHUNK_SIZE * 2  # 576 chars per chunk
    for i in range(4):
        chunk = art_hex[i * chunk_hex_size : (i + 1) * chunk_hex_size]
        cmd = f"ART:{i}:{chunk}\n"
        ser.write(cmd.encode("ascii"))
        ser.flush()
        if not wait_for_ack(ser, "ACK_ART", timeout=3.0):
            print(f"  [WARN] Timeout menunggu ACK untuk ART chunk {i}")
        time.sleep(0.02)

    ser.write(b"ART_END\n")
    ser.flush()
    if not wait_for_ack(ser, "ACK_DRAW", timeout=10.0):
        print("  [WARN] Timeout menunggu ACK_DRAW dari ESP32")


# ============================================================================
# Lyrics (LRCLIB dengan auto-clear saat jeda/solo musik)
# ============================================================================
class LyricTracker:
    """Mengambil dan melacak lirik sinkron dari LRCLIB (hanya baris aktif, kosong saat jeda/solo)."""

    def __init__(self):
        self.lines = []  # list of (time_ms, text)
        self.last_emitted_text = None
        self.loaded_key = ""

    def fetch(self, title, artist, duration_sec):
        """Ambil lirik dari LRCLIB: coba get langsung, jika gagal gunakan search."""
        key = f"{title}|{artist}"
        if key == self.loaded_key:
            return

        self.lines = []
        self.last_emitted_text = None
        self.loaded_key = key

        # 1. Coba direct get
        params = urllib.parse.urlencode({
            "track_name": title,
            "artist_name": artist,
            "duration": int(duration_sec),
        })
        url = f"https://lrclib.net/api/get?{params}"
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "SpotifyDesk/1.0"})
            with urllib.request.urlopen(req, timeout=4) as resp:
                data = json.loads(resp.read().decode("utf-8"))
                synced = data.get("syncedLyrics", "")
                if synced:
                    self._parse_lrc(synced)
                    print(f"  [LYRIC] {len(self.lines)} baris lirik sinkron dimuat (Direct)")
                    return
        except Exception:
            pass

        # 2. Fallback search (bersihkan judul dari embel-embel Remaster/Live)
        search_query = f"{clean_title_for_search(title)} {artist}".strip()
        params = urllib.parse.urlencode({"q": search_query})
        url = f"https://lrclib.net/api/search?{params}"
        try:
            req = urllib.request.Request(url, headers={"User-Agent": "SpotifyDesk/1.0"})
            with urllib.request.urlopen(req, timeout=4) as resp:
                items = json.loads(resp.read().decode("utf-8"))
                for item in items:
                    synced = item.get("syncedLyrics", "")
                    if synced:
                        self._parse_lrc(synced)
                        print(f"  [LYRIC] {len(self.lines)} baris lirik sinkron dimuat (Search: {item.get('trackName')})")
                        return
        except Exception as e:
            print(f"  [LYRIC] Pencarian lirik gagal: {e}")

        print("  [LYRIC] Lirik tidak ditemukan di LRCLIB (area lirik dibiarkan kosong)")

    def _parse_lrc(self, lrc_text):
        """Parse format LRC: [mm:ss.xx] teks lirik"""
        self.lines = []
        for m in re.finditer(r"\[(\d+):(\d+)\.(\d+)\]\s*(.*)", lrc_text):
            minutes = int(m.group(1))
            seconds = int(m.group(2))
            dec_fraction = float(f"0.{m.group(3)}")
            time_ms = int((minutes * 60 + seconds) * 1000 + dec_fraction * 1000)
            text = clean_text_for_display(m.group(4))
            # Abaikan baris kosong atau simbol musik instrumen agar self.lines murni baris vokal
            if text and text not in ["♪", "♫", "---", "..."] and not re.match(r"^[\(\[]?(music|instrumental|solo)[\)\]]?$", text, re.I):
                self.lines.append((time_ms, text))
        self.lines.sort(key=lambda x: x[0])

    def _compute_current_text(self, pos_ms):
        """Hitung lirik yang seharusnya tampil pada pos_ms saat ini."""
        if not self.lines:
            return ""

        idx = -1
        for i, (t, _) in enumerate(self.lines):
            if t <= pos_ms:
                idx = i
            else:
                break

        # Belum masuk lirik pertama (intro lagu -> tampilkan simbol not balok musik)
        if idx < 0:
            return ""

        curr_t, text = self.lines[idx]
        elapsed_in_line = pos_ms - curr_t

        # Cek apakah ada baris berikutnya
        if idx + 1 < len(self.lines):
            next_t = self.lines[idx + 1][0]
            gap = next_t - curr_t

            # Hanya anggap sebagai melodi / jeda instrumental jika jeda >= 20 detik (solo musik sejati)!
            # Untuk jeda normal antar baris vokal (< 20 detik), lirik tetap tampil penuh sampai baris berikutnya.
            if gap >= 20000:
                display_dur = max(10000, min(gap - 5000, len(text) * 350))
                if elapsed_in_line >= display_dur:
                    return ""
        else:
            # Baris terakhir lagu (outro): beri waktu 10 detik sebelum berganti ke not balok
            if elapsed_in_line >= 10000:
                return ""

        return text

    def get_current_line(self, elapsed_pos):
        """Kembalikan string teks baru jika ada perubahan status, atau None jika tidak berubah.
        elapsed_pos bisa berupa float (detik) atau int dengan presisi milidetik."""
        if not self.lines:
            text = ""
        else:
            pos_ms = int(float(elapsed_pos) * 1000) + LYRIC_LEAD_MS
            text = self._compute_current_text(pos_ms)

        if text != self.last_emitted_text:
            self.last_emitted_text = text
            return text

        return None

    def reset(self):
        self.lines = []
        self.last_emitted_text = None
        self.loaded_key = ""


# ============================================================================
# Windows Media Session
# ============================================================================
async def get_media_info(manager):
    """Ambil info media aktif dari Windows Media Session dengan kalkulasi waktu real-time."""
    sessions = manager.get_sessions()
    session = None

    # Prioritaskan Spotify jika sedang terbuka
    for s in sessions:
        if "spotify" in (s.source_app_user_model_id or "").lower():
            session = s
            break

    if not session:
        session = manager.get_current_session()
        if not session and len(sessions) > 0:
            session = sessions[0]

    if not session:
        return None

    props = await session.try_get_media_properties_async()
    if not props:
        return None

    playback = session.get_playback_info()
    is_playing = False
    if playback and playback.playback_status:
        is_playing = (playback.playback_status.value == 4)

    # Hitung posisi waktu real-time yang sebenarnya (presisi milidetik untuk sinkronisasi lirik)
    timeline = session.get_timeline_properties()
    elapsed_sec = 0
    duration_sec = 0
    pos_exact = 0.0
    if timeline:
        pos = timeline.position.total_seconds() if timeline.position else 0.0
        if is_playing and timeline.last_updated_time:
            now_utc = datetime.now(timezone.utc)
            delta = (now_utc - timeline.last_updated_time).total_seconds()
            if delta > 0:
                pos += delta
        pos_exact = pos
        elapsed_sec = int(pos)
        if timeline.end_time:
            duration_sec = int(timeline.end_time.total_seconds())

    # Ambil cover art thumbnail
    thumb_bytes = None
    if props.thumbnail:
        try:
            stream = await props.thumbnail.open_read_async()
            size = stream.size
            if size > 0:
                reader = DataReader(stream)
                await reader.load_async(size)
                buf = bytearray(size)
                reader.read_bytes(buf)
                thumb_bytes = bytes(buf)
        except Exception:
            pass

    return {
        "title": props.title or "",
        "artist": props.artist or "",
        "album": props.album_title or "",
        "is_playing": is_playing,
        "elapsed": elapsed_sec,
        "elapsed_exact": pos_exact,
        "duration": duration_sec,
        "thumb_bytes": thumb_bytes,
    }


# ============================================================================
# Main Loop
# ============================================================================
async def main():
    print("=" * 60)
    print("  SpotifyDesk - Desktop Companion (Windows)")
    print("=" * 60)

    port = find_esp32_port()
    if not port:
        print("[ERROR] ESP32 tidak ditemukan! Pastikan kabel USB terpasang.")
        return

    print(f"[*] Menghubungkan ke ESP32 pada {port}...")
    try:
        ser = serial.Serial(port, BAUD_RATE, timeout=0.5)
        time.sleep(2)  # Menunggu ESP32 selesai reset DTR
    except Exception as e:
        print(f"[ERROR] Gagal membuka port {port}: {e}")
        return

    # Buang teks boot yang tersisa
    while ser.in_waiting:
        ser.readline()

    print("[*] Terhubung ke ESP32!")
    print("[*] Menginisialisasi Windows Media Manager...")
    manager = await MediaManager.request_async()
    lyrics = LyricTracker()

    last_track_key = None
    last_sync_time = 0
    last_playing = False
    idle_sent = False

    print()
    print("[OK] Companion aktif! (Tekan Ctrl+C untuk berhenti)")
    print()

    try:
        while True:
            info = await get_media_info(manager)

            # Tidak ada media aktif -> Standby
            if not info or (not info["title"] and not info["artist"]):
                if not idle_sent:
                    print("[INFO] Tidak ada lagu aktif. Mengirim mode Standby...")
                    ser.write(b"IDLE\n")
                    ser.flush()
                    idle_sent = True
                    last_track_key = None
                    lyrics.reset()
                await asyncio.sleep(1)
                continue

            idle_sent = False
            track_key = f"{info['title']}|{info['artist']}"

            # ============================================================
            # Lagu berganti -> Ambil lirik & art, lalu kirim ke ESP32
            # ============================================================
            if track_key != last_track_key:
                last_track_key = track_key
                status = "PLAYING" if info["is_playing"] else "PAUSED"
                print(f"\n[Lagu Baru] [{status}] {info['title']} - {info['artist']}")
                if info["album"]:
                    print(f"            Album : {info['album']}")
                dur_m, dur_s = divmod(info["duration"], 60)
                print(f"            Durasi: {dur_m}:{dur_s:02d}")

                # 1. Bersihkan teks
                t = clean_text_for_display(info["title"])
                a = clean_text_for_display(info["artist"])
                al = clean_text_for_display(info["album"])
                pv = 1 if info["is_playing"] else 0
                last_playing = info["is_playing"]

                # 2. Ambil lirik lebih dulu agar bisa digambar bersamaan
                print("            Mengambil lirik (LRCLIB)...")
                if info["duration"] > 0:
                    lyrics.fetch(info["title"], info["artist"], info["duration"])
                else:
                    lyrics.reset()

                # Ambil 1 baris lirik awal (atau kosong jika masih intro)
                init_lyric = lyrics.get_current_line(info.get("elapsed_exact", info["elapsed"])) or ""
                ltext = clean_text_for_display(init_lyric)

                # 3. Kirim META
                meta = f"META|{pv}|{info['elapsed']}|{info['duration']}|{t}|{a}|{al}\n"
                ser.write(meta.encode("utf-8"))
                ser.flush()
                wait_for_ack(ser, "ACK_META", timeout=2.0)

                # 4. Kirim lirik awal ke buffer ESP32
                lcmd = f"LYRIC|{ltext}\n"
                ser.write(lcmd.encode("utf-8"))
                ser.flush()

                # 5. Proses dan kirim cover art (4 chunk)
                art_hex = ""
                if info["thumb_bytes"]:
                    print("            Memproses cover art (Floyd-Steinberg 96x96)...")
                    art_hex = process_thumbnail_to_hex(info["thumb_bytes"])
                    if art_hex:
                        print("            Mengirim cover art (4 chunk)...")

                send_art_chunked(ser, art_hex)
                print("            [OK] Layar berhasil diperbarui!")

                last_sync_time = time.time()
                await asyncio.sleep(1)
                continue

            # ============================================================
            # Lagu sedang berjalan -> SYNC periodik + update lirik
            # ============================================================
            now = time.time()

            # Periksa pesan balasan dari ESP32 (misal ESP32 baru reboot dan minta META ulang)
            while ser.in_waiting:
                try:
                    resp = ser.readline().decode("ascii", errors="replace").strip()
                    if "REQ_META" in resp:
                        print("\n[*] ESP32 meminta sinkronisasi ulang data lagu...")
                        last_track_key = None  # Paksa kirim ulang META, cover art, dan lirik!
                        break
                except Exception:
                    pass

            # SYNC posisi waktu setiap 5 detik atau segera jika status Play/Pause berubah
            play_changed = (last_playing != info["is_playing"])
            if play_changed or (now - last_sync_time >= SYNC_INTERVAL_SEC):
                last_sync_time = now
                last_playing = info["is_playing"]
                pv = 1 if info["is_playing"] else 0
                sync_cmd = f"SYNC|{pv}|{info['elapsed']}|{info['duration']}\n"
                ser.write(sync_cmd.encode("ascii"))
                ser.flush()

            # Cek perubahan lirik yang sedang dinyanyikan (atau not balok saat jeda melodi/intro)
            if lyrics.lines or lyrics.last_emitted_text is None:
                new_lyric = lyrics.get_current_line(info.get("elapsed_exact", info["elapsed"]))
                if new_lyric is not None:
                    ltext = clean_text_for_display(new_lyric)
                    lcmd = f"LYRIC|{ltext}\n"
                    ser.write(lcmd.encode("utf-8"))
                    ser.flush()
                    if ltext:
                        print(f"\n  [LYRIC] -> \"{ltext}\"")
                    else:
                        print(f"\n  [LYRIC] -> [Melodi / Not Balok]")

            # Tampilan progres di konsol
            si = "[>]" if info["is_playing"] else "[||]"
            me, se = divmod(info["elapsed"], 60)
            md, sd = divmod(info["duration"], 60)
            sys.stdout.write(f"\r  {si} {me}:{se:02d} / {md}:{sd:02d}   ")
            sys.stdout.flush()

            await asyncio.sleep(0.1)

    except KeyboardInterrupt:
        print("\n\n[*] Companion dihentikan.")
    finally:
        ser.close()
        print("[*] Serial port ditutup.")


if __name__ == "__main__":
    asyncio.run(main())
