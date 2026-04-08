## #!/usr/bin/env python3
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
#             data = {"roller_speed": 0, "mode": 0}
#     # Normalize missing keys
#     return {
#         "roller_speed": int(data.get("roller_speed", 0)),
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
HOST = "192.168.10.1"   # As confirmed
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

def load_state() -> Dict:
    """
    Read the shared JSON written by the poll script and normalize it into
    a flat dict of numeric values suitable for CSV output.

    Expected JSON schema:
      {
        "ready_to_run": bool,
        "active_variety": int or null,
        "1": {
          "roller_speed": int,
          "belt_speed": int,
          "irrigation_delay": int,
          "irrigation_duration": int,
          "misting_delay": int,
          "misting_duration": int,
          "roller_delay": int,
          "roller_duration": int
        },
        "2": { ... },
        ...
      }
    """
    with FileLock(LOCK_FILE_PATH, shared=True):
        try:
            with open(JSON_FILE_PATH, "r") as f:
                data = json.load(f)
        except (FileNotFoundError, json.JSONDecodeError):
            data = {}

    ready_to_run = int(bool(data.get("ready_to_run", False)))
    active_variety = data.get("active_variety", None)

    # Default values if nothing is set yet
    variety_values = {
        "roller_speed": 0,
        "belt_speed": 0,
        "irrigation_delay": 0,
        "irrigation_duration": 0,
        "misting_delay": 0,
        "misting_duration": 0,
        "roller_delay": 0,
        "roller_duration": 0,
    }

    if active_variety is not None:
        key = str(active_variety)
        v = data.get(key, {})
        if isinstance(v, dict):
            for k in variety_values.keys():
                try:
                    variety_values[k] = int(v.get(k, 0))
                except (TypeError, ValueError):
                    variety_values[k] = 0
    else:
        active_variety = -1  # sentinel for "no active variety"

    return {
        "ready_to_run": ready_to_run,
        "active_variety": int(active_variety),
        "roller_speed": variety_values["roller_speed"],
        "belt_speed": variety_values["belt_speed"],
        "irrigation_delay": variety_values["irrigation_delay"],
        "irrigation_duration": variety_values["irrigation_duration"],
        "misting_delay": variety_values["misting_delay"],
        "misting_duration": variety_values["misting_duration"],
        "roller_delay": variety_values["roller_delay"],
        "roller_duration": variety_values["roller_duration"],
    }

def serve():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        # Allow quick restart after crash
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((HOST, PORT))
        s.listen(5)
        print(f"tcp: serving on {HOST}:{PORT}")

        while True:
            conn, addr = s.accept()
            with conn:
                try:
                    state = load_state()

                    # CSV payload in a fixed order for ClearCore or other client
                    payload_fields = [
                        state["ready_to_run"],
                        state["active_variety"],
                        state["roller_speed"],
                        state["belt_speed"],
                        state["irrigation_delay"],
                        state["irrigation_duration"],
                        state["misting_delay"],
                        state["misting_duration"],
                        state["roller_delay"],
                        state["roller_duration"],
                    ]
                    payload = ",".join(str(x) for x in payload_fields)

                    conn.sendall(payload.encode("utf-8"))
                    # Optional: add newline if client expects it
                    # conn.sendall(b"\n")

                    print(f"tcp: {addr} -> '{payload}'")

                except Exception as e:
                    try:
                        conn.sendall(b"ERR")
                    except Exception:
                        pass
                    print(f"tcp: error serving {addr}: {e}")

if __name__ == "__main__":
    serve() #!/usr/bin/env python3
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
#             data = {"roller_speed": 0, "mode": 0}
#     # Normalize missing keys
#     return {
#         "roller_speed": int(data.get("roller_speed", 0)),
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
HOST = "192.168.10.1"   # As confirmed
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

def load_state() -> Dict:
    """
    Read the shared JSON written by the poll script and normalize it into
    a flat dict of numeric values suitable for CSV output.

    Expected JSON schema:
      {
        "ready_to_run": bool,
        "active_variety": int or null,
        "1": {
          "roller_speed": int,
          "belt_speed": int,
          "irrigation_delay": int,
          "irrigation_duration": int,
          "misting_delay": int,
          "misting_duration": int,
          "roller_delay": int,
          "roller_duration": int
        },
        "2": { ... },
        ...
      }
    """
    with FileLock(LOCK_FILE_PATH, shared=True):
        try:
            with open(JSON_FILE_PATH, "r") as f:
                data = json.load(f)
        except (FileNotFoundError, json.JSONDecodeError):
            data = {}

    ready_to_run = int(bool(data.get("ready_to_run", False)))
    active_variety = data.get("active_variety", None)

    # Default values if nothing is set yet
    variety_values = {
        "roller_speed": 0,
        "belt_speed": 0,
        "irrigation_delay": 0,
        "irrigation_duration": 0,
        "misting_delay": 0,
        "misting_duration": 0,
        "roller_delay": 0,
        "roller_duration": 0,
    }

    if active_variety is not None:
        key = str(active_variety)
        v = data.get(key, {})
        if isinstance(v, dict):
            for k in variety_values.keys():
                try:
                    variety_values[k] = int(v.get(k, 0))
                except (TypeError, ValueError):
                    variety_values[k] = 0
    else:
        active_variety = -1  # sentinel for "no active variety"

    return {
        "ready_to_run": ready_to_run,
        "active_variety": int(active_variety),
        "roller_speed": variety_values["roller_speed"],
        "belt_speed": variety_values["belt_speed"],
        "irrigation_delay": variety_values["irrigation_delay"],
        "irrigation_duration": variety_values["irrigation_duration"],
        "misting_delay": variety_values["misting_delay"],
        "misting_duration": variety_values["misting_duration"],
        "roller_delay": variety_values["roller_delay"],
        "roller_duration": variety_values["roller_duration"],
    }

def serve():
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        # Allow quick restart after crash
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((HOST, PORT))
        s.listen(5)
        print(f"tcp: serving on {HOST}:{PORT}")

        while True:
            conn, addr = s.accept()
            with conn:
                try:
                    state = load_state()

                    # CSV payload in a fixed order for ClearCore or other client
                    payload_fields = [
                        state["ready_to_run"],
                        state["active_variety"],
                        state["roller_speed"],
                        state["belt_speed"],
                        state["irrigation_delay"],
                        state["irrigation_duration"],
                        state["misting_delay"],
                        state["misting_duration"],
                        state["roller_delay"],
                        state["roller_duration"],
                    ]
                    payload = ",".join(str(x) for x in payload_fields)

                    conn.sendall(payload.encode("utf-8"))
                    # Optional: add newline if client expects it
                    # conn.sendall(b"\n")

                    print(f"tcp: {addr} -> '{payload}'")

                except Exception as e:
                    try:
                        conn.sendall(b"ERR")
                    except Exception:
                        pass
                    print(f"tcp: error serving {addr}: {e}")

if __name__ == "__main__":
    serve()
