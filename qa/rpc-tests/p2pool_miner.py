#!/usr/bin/env python3
# Copyright (c) 2025 The Juno Cash developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or https://www.opensource.org/licenses/mit-license.php .

#
# Test native P2Pool miner integration
#

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    start_nodes,
    connect_nodes_bi,
)

import http.server
import socketserver
import threading
import json
import time
import struct

# Mock P2Pool Server
class MockP2PoolHandler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        content_len = int(self.headers.get('Content-Length'))
        post_body = self.rfile.read(content_len)
        request = json.loads(post_body)
        
        method = request['method']
        params = request['params']
        
        response = {
            "jsonrpc": "2.0",
            "result": None,
            "error": None,
            "id": request['id']
        }
        
        if method == "get_share_template":
            # Return a dummy template
            # Header must be 140 bytes hex
            header = "04000000" + "00"*32 + "00"*32 + "00"*32 + "00000000" + "ffff0f1e" + "00"*32 + "00" # Dummy header
            # Pad to 140 bytes (280 hex chars)
            header = header.ljust(280, '0')
            
            response['result'] = {
                "blocktemplate_blob": header,
                "seed_hash": "00"*32,
                "difficulty": 100,
                "height": 100,
                "target": "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff" # Max target
            }
            self.server.template_requests += 1
            
        elif method == "submit_share":
            self.server.share_submissions += 1
            response['result'] = True
            
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.end_headers()
        self.wfile.write(json.dumps(response).encode('utf-8'))

    def log_message(self, format, *args):
        pass # Silence logs

class P2PoolMinerTest(BitcoinTestFramework):
    def __init__(self):
        super().__init__()
        self.num_nodes = 1
        self.setup_clean_chain = True

    def setup_network(self, split=False):
        # Start mock server first
        # Bind to port 0 to let OS assign a free port
        self.mock_server = socketserver.TCPServer(('127.0.0.1', 0), MockP2PoolHandler)
        self.mock_port = self.mock_server.server_address[1]
        self.mock_server.template_requests = 0
        self.mock_server.share_submissions = 0
        
        self.server_thread = threading.Thread(target=self.mock_server.serve_forever)
        self.server_thread.daemon = True
        self.server_thread.start()
        print(f"Mock P2Pool server started on port {self.mock_port}")

        # Start node with p2pool configuration
        self.nodes = start_nodes(self.num_nodes, self.options.tmpdir,
                                 extra_args=[[
                                     f'-p2poolurl=http://127.0.0.1:{self.mock_port}',
                                     '-p2pooladdress=tmCRpLQqPHMafVbWnKExesTHKGP4FqBDMfN',
                                     '-gen=1',
                                     '-genproclimit=1',
                                     '-randomxfastmode=0' # Light mode for testing
                                 ]] * self.num_nodes)

    def run_test(self):
        print("Testing native P2Pool miner...")
        
        node = self.nodes[0]
        
        # Wait for miner to start and poll
        print("Waiting for miner to poll P2Pool...")
        start_time = time.time()
        while self.mock_server.template_requests < 5:
            if time.time() - start_time > 30:
                raise AssertionError("Miner did not poll P2Pool server")
            time.sleep(1)
            
        print(f"✓ Miner polling detected ({self.mock_server.template_requests} requests)")
        
        # Wait for share submission
        # Since target is max, it should find shares quickly
        print("Waiting for share submission...")
        start_time = time.time()
        while self.mock_server.share_submissions == 0:
            if time.time() - start_time > 60:
                print("TIMEOUT: No shares submitted. Dumping debug.log:")
                log_path = self.options.tmpdir + "/node0/regtest/debug.log"
                with open(log_path, 'r', encoding='utf-8', errors='ignore') as f:
                    print(f.read())
                raise AssertionError("Miner failed to submit shares")
            
            if self.mock_server.share_submissions > 0:
                break
            time.sleep(1)
            
        if self.mock_server.share_submissions > 0:
            print(f"✓ Share submission detected ({self.mock_server.share_submissions} shares)")
        else:
            print("⚠ No shares submitted (miner might be initializing or slow)")
            
        # Verify mining info
        info = node.getmininginfo()
        print(f"Mining info: {info}")
        
        self.mock_server.shutdown()
        self.server_thread.join()

if __name__ == '__main__':
    P2PoolMinerTest().main()
