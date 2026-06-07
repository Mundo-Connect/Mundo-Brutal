import argparse
import socket
import struct
import threading
import time

TCP_CONGESTION = 13
TCP_BRUTAL_PARAMS = 23301
DEFAULT_PORT = 65432
DEFAULT_BUFFER_SIZE = 65536


def mbps_to_bytes(value):
    return int(value * 1000 * 1000 / 8)


def configure_mundo(conn, max_rate_bps, cwnd_gain):
    conn.setsockopt(socket.IPPROTO_TCP, TCP_CONGESTION, b"mundo")
    params = struct.pack("=QI", max_rate_bps, cwnd_gain)
    conn.setsockopt(socket.IPPROTO_TCP, TCP_BRUTAL_PARAMS, params)


def client_thread(conn, addr, duration, buffer_size, max_rate_bps, cwnd_gain):
    print(f"Connected by {addr}, negotiated max {max_rate_bps * 8 / 1000000:.2f} Mbps")
    configure_mundo(conn, max_rate_bps, cwnd_gain)

    payload = bytearray(buffer_size)
    started = time.time()
    try:
        while time.time() - started < duration:
            conn.sendall(payload)
    except Exception as exc:
        print(f"Error sending to {addr}: {exc}")
    finally:
        conn.close()
        print(f"Disconnected {addr}")


def main():
    parser = argparse.ArgumentParser(description="TCP Mundo example server")
    parser.add_argument("-l", "--listen", default="", help="Address to listen on")
    parser.add_argument("-p", "--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("-d", "--duration", type=int, default=30)
    parser.add_argument("-b", "--buffer-size", type=int, default=DEFAULT_BUFFER_SIZE)
    parser.add_argument("--server-max-mbps", type=int, required=True)
    parser.add_argument("--cwnd-gain", type=int, default=18)
    args = parser.parse_args()

    server_max_bps = mbps_to_bytes(args.server_max_mbps)

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.setsockopt(socket.IPPROTO_TCP, TCP_CONGESTION, b"mundo")
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind((args.listen, args.port))
        listener.listen()
        print(f"Server listening on {args.listen}:{args.port}")

        try:
            while True:
                conn, addr = listener.accept()
                raw = conn.recv(4)
                if len(raw) != 4:
                    conn.close()
                    continue

                client_limit_mbps = struct.unpack("!I", raw)[0]
                negotiated_bps = min(server_max_bps, mbps_to_bytes(client_limit_mbps))
                thread = threading.Thread(
                    target=client_thread,
                    args=(
                        conn,
                        addr,
                        args.duration,
                        args.buffer_size,
                        negotiated_bps,
                        args.cwnd_gain,
                    ),
                    daemon=True,
                )
                thread.start()
        except KeyboardInterrupt:
            print("\nServer is shutting down.")


if __name__ == "__main__":
    main()
