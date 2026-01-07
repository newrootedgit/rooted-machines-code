# #!/usr/bin/env python3
# import os
# import json
# import socket
# import fcntl
# from typing import Dict

# # ========================
# # Configuration
# # ========================
# HOST = "192.168.10.1"   # As confirmed
# PORT = 8888
# JSON_FILE_PATH = "/home/rooted/te-cli/TE_Variable_Values.json"
# LOCK_FILE_PATH = JSON_FILE_PATH + ".lock"

# # ========================
# # Lock helpers (advisory)
# # ========================
# class FileLock:
#     def __init__(self, lock_path: str, shared: bool):
#         self.lock_path = lock_path
#         self.shared = shared
#         self._fh = None

#     def __enter__(self):
#         self._fh = open(self.lock_path, "a+")
#         fcntl.flock(self._fh, fcntl.LOCK_SH if self.shared else fcntl.LOCK_EX)
#         return self

#     def __exit__(self, exc_type, exc, tb):
#         try:
#             fcntl.flock(self._fh, fcntl.LOCK_UN)
#         finally:
#             self._fh.close()
#             self._fh = None

# def load_state() -> Dict:
#     with FileLock(LOCK_FILE_PATH, shared=True):
#         try:
#             with open(JSON_FILE_PATH, "r") as f:
#                 data = json.load(f)
#         except (FileNotFoundError, json.JSONDecodeError):
#             data = {"belt_speed": 0, "mode": 0}
#     # Normalize missing keys
#     return {
#         "belt_speed": int(data.get("belt_speed", 0)),
#         "mode": int(data.get("mode", 0)),
#     }

# def serve():
#     with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
#         # Allow quick restart after crash
#         s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
#         s.bind((HOST, PORT))
#         s.listen(5)
#         print(f"tcp: serving on {HOST}:{PORT}")

#         while True:
#             conn, addr = s.accept()
#             with conn:
#                 try:
#                     state = load_state()
#                     payload = f'{state["roller_speed"]},{state["mode"]}'
#                     conn.sendall(payload.encode("utf-8"))
#                     # Optional: add newline for convenience
#                     # conn.sendall(b"\n")
#                     print(f"tcp: {addr} -> '{payload}'")
#                 except Exception as e:
#                     try:
#                         conn.sendall(b"ERR")
#                     except Exception:
#                         pass
#                     print(f"tcp: error serving {addr}: {e}")

# if __name__ == "__main__":
#     serve()

































#!/usr/bin/env python3
import os
import json
import socket
import fcntl
from typing import Dict

# ========================
# Configuration
# ========================
HOST = "0.0.0.0"          # bind all interfaces (use "192.168.10.1" if you want to bind only that IP)
PORT = 8888
JSON_FILE_PATH = "/home/rooted/te-cli/TE_Variable_Values.json"
LOCK_FILE_PATH = JSON_FILE_PATH + ".lock"

# ========================
# Lock helpers (advisory)
# ========================
class FileLock:
    def __init__(self, lock_path: str, shared: bool):
        self.lock_path = lock_path
        self.shared = shared
        self._fh = None

    def __enter__(self):
        self._fh = open(self.lock_path, "a+")
        fcntl.flock(self._fh, fcntl.LOCK_SH if self.shared else fcntl.LOCK_EX)
        return self

    def __exit__(self, exc_type, exc, tb):
        try:
            fcntl.flock(self._fh, fcntl.LOCK_UN)
        finally:
            self._fh.close()
            self._fh = None

def load_state() -> Dict[str, int]:
    with FileLock(LOCK_FILE_PATH, shared=True):
        try:
            with open(JSON_FILE_PATH, "r") as f:
                data = json.load(f)
        except (FileNotFoundError, json.JSONDecodeError):
            data = {"belt_speed": 0, "mode": 0}

    # Normalize and coerce
    try:
        belt_speed = int(data.get("belt_speed", 0))
    except (TypeError, ValueError):
        belt_speed = 0
    try:
        mode = int(data.get("mode", 0))
    except (TypeError, ValueError):
        mode = 0

    return {"belt_speed": belt_speed, "mode": mode}

def serve():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((HOST, PORT))
        s.listen(5)
        print(f"tcp: serving on {HOST}:{PORT}")

        while True:
            conn, addr = s.accept()
            with conn:
                try:
                    state = load_state()
                    # Build payload from belt_speed and mode
                    payload = f'{state["belt_speed"]},{state["mode"]}'
                    conn.sendall(payload.encode("utf-8"))
                    # Optional convenience newline:
                    # conn.sendall(b"\n")
                    print(f"tcp: {addr} -> state={state} payload='{payload}'")
                except Exception as e:
                    try:
                        conn.sendall(b"ERR")
                    except Exception:
                        pass
                    print(f"tcp: error serving {addr}: {e}")

if __name__ == "__main__":
    serve()
