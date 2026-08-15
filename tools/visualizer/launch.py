#!/usr/bin/env python3
import http.server
import socketserver
import os
import sys

PORT = 8080
DIRECTORY = os.path.dirname(os.path.abspath(__file__))

class Handler(http.server.SimpleHTTPRequestHandler):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=DIRECTORY, **kwargs)

def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else PORT
    with socketserver.TCPServer(("", port), Handler) as httpd:
        print("=" * 66)
        print("  Alg-SDK Pipeline & DAG Visualizer Server Started")
        print(f"  Access URL: http://localhost:{port}/index.html")
        print("  Press Ctrl+C to stop the server.")
        print("=" * 66)
        try:
            httpd.serve_forever()
        except KeyboardInterrupt:
            print("\nShutting down visualizer server.")

if __name__ == "__main__":
    main()
