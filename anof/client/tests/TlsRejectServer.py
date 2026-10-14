"""Private, bounded TLS peer; fixture key is public test data, never a server key."""
import pathlib
import socket
import ssl
import sys

fixtures = pathlib.Path(__file__).parent / "fixtures"
context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(fixtures / "untrusted-cert.pem", fixtures / "untrusted-key.pem")
peer = socket.socket(fileno=0)
peer.settimeout(2)
try:
    with context.wrap_socket(peer, server_side=True):
        sys.exit(2)  # Accepting this fixture would violate the client trust policy.
except ssl.SSLError:
    sys.exit(0)
except OSError:
    sys.exit(3)
