import argparse
import socket
import time

DEFAULT_PORT = 65432
DEFAULT_BUFFER_SIZE = 65536


def main(host, port, buf_size, limit_mbps):
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.connect((host, port))
        sock.sendall(limit_mbps.to_bytes(4, byteorder="big"))
        print(f"Connected to {host}:{port}, client limit {limit_mbps} Mbps")

        counter = 0
        started = time.time()
        try:
            while True:
                data = sock.recv(buf_size)
                if not data:
                    break
                counter += len(data)
                now = time.time()
                if now - started >= 1:
                    mbps = counter * 8 / 1000000 / (now - started)
                    print(f"Current speed: {mbps:.2f} Mbps")
                    counter = 0
                    started = now
        except KeyboardInterrupt:
            print("\nInterrupted by user")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Mundo X Brutal example client")
    parser.add_argument("host")
    parser.add_argument("limit_mbps", type=int)
    parser.add_argument("-p", "--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("-b", "--buffer", type=int, default=DEFAULT_BUFFER_SIZE)
    args = parser.parse_args()

    main(args.host, args.port, args.buffer, args.limit_mbps)
