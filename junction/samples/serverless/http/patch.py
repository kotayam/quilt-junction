import socket

def patch_socket():
    _original_setsockopt = socket.socket.setsockopt

    def patched_setsockopt(self, level, optname, value):
        try:
            _original_setsockopt(self, level, optname, value)
        except OSError:
            # Ignore "Invalid argument" or "Protocol not available" from the runtime
            pass

    socket.socket.setsockopt = patched_setsockopt
