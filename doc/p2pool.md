# P2Pool Mining Support for JunoCash

## Overview

JunoCash implements three RPC methods specifically designed for p2pool mining:
- `getminerdata` - Lightweight blockchain state query
- `calc_pow` - External RandomX hash calculation
- `add_aux_pow` - Merge mining support

These methods enable decentralized pool mining while maintaining the security and efficiency of JunoCash's RandomX proof-of-work system.

## RPC Methods

### getminerdata

Returns essential blockchain state for constructing p2pool sidechain blocks. This is a lightweight alternative to `getblocktemplate` that provides only the data needed for p2pool operation.

#### Request
```bash
junocash-cli getminerdata
```

No parameters required.

#### Response
```json
{
  "version": 4,
  "height": 12345,
  "prevhash": "0000000000...",
  "randomxseedheight": 12288,
  "randomxseedhash": "abc123...",
  "bits": "1d00ffff",
  "difficulty": 1024.5,
  "mediantime": 1704067200,
  "blockcommitmentshash": "null",
  "tx_backlog": [
    {
      "txid": "def456...",
      "size": 250,
      "fee": 10000
    }
  ]
}
```

#### Fields

| Field | Type | Description |
|-------|------|-------------|
| version | numeric | Block version for next block |
| height | numeric | Next block height |
| prevhash | string | Hash of current chain tip |
| randomxseedheight | numeric | Block height used for RandomX seed |
| randomxseedhash | string | RandomX seed hash (or 0x08... for genesis) |
| bits | string | Difficulty target in compact format |
| difficulty | numeric | Current network difficulty |
| mediantime | numeric | Median time past of previous blocks |
| blockcommitmentshash | string | Block commitments (NU5+), null for p2pool |
| tx_backlog | array | Mempool transactions with txid, size, fee |

#### Usage Pattern

P2pool should poll this endpoint every 1-5 seconds to detect new blocks and update sidechain state.

```python
# Example p2pool usage
while True:
    miner_data = rpc.getminerdata()

    if miner_data['height'] != current_height:
        # New block detected, update sidechain
        update_sidechain(miner_data)
        current_height = miner_data['height']

    # Build sidechain blocks using miner_data
    # and tx_backlog for transaction selection

    time.sleep(2)  # Poll every 2 seconds
```

### calc_pow

Calculates the RandomX proof-of-work hash for a given block. This allows p2pool and external miners to delegate RandomX hashing to the daemon, avoiding the need to implement RandomX themselves.

#### Request
```bash
# With auto-detection
junocash-cli calc_pow "hexblockdata"

# With explicit seed hash
junocash-cli calc_pow "hexblockdata" "randomxseedhash"
```

#### Parameters

1. `hexdata` (string, required) - Hex-encoded block header or full block
2. `seedhash` (string, optional) - RandomX seed hash (auto-detected if omitted)

#### Response
```
"a1b2c3d4e5f6..."
```

Returns the RandomX hash as a hex string.

#### Usage Pattern

```python
# Build block header
block_header = construct_block_header(
    version=miner_data['version'],
    prevhash=miner_data['prevhash'],
    merkleroot=calculate_merkle_root(transactions),
    timestamp=int(time.time()),
    bits=miner_data['bits'],
    nonce=random_nonce()
)

# Calculate PoW hash via daemon
header_hex = block_header.hex()
seed_hash = miner_data['randomxseedhash']
pow_hash = rpc.calc_pow(header_hex, seed_hash)

# Check if hash meets target
if int(pow_hash, 16) < target:
    # Submit block!
    submit_block(block)
```

#### Performance Notes

- RandomX hashing is CPU-intensive (~10-100ms per hash depending on CPU)
- Consider caching RandomX dataset in daemon for optimal performance
- Use `-randomxfastmode=1` for faster hashing with higher memory usage

### add_aux_pow

Enables merge mining by embedding a merkle root of multiple chain block hashes into the coinbase transaction. This allows mining JunoCash simultaneously with other blockchains.

#### Request
```bash
junocash-cli add_aux_pow '{
  "blocktemplate_blob": "hexblockdata",
  "aux_pow": [
    {"id": "chain1_id_hex", "hash": "chain1_block_hash"},
    {"id": "chain2_id_hex", "hash": "chain2_block_hash"}
  ]
}'
```

#### Parameters

1. `request` (object, required)
   - `blocktemplate_blob` (string) - Hex-encoded block from getblocktemplate
   - `aux_pow` (array) - Auxiliary chain data
     - `id` (string) - Chain identifier (hash)
     - `hash` (string) - Block hash from auxiliary chain

#### Response
```json
{
  "blocktemplate_blob": "updated_hex_block_data",
  "blockhashing_blob": "block_header_hex",
  "merkle_root": "abc123...",
  "merkle_tree_depth": 65536,
  "aux_pow": [
    {"id": "chain1_id", "hash": "chain1_hash"},
    {"id": "chain2_id", "hash": "chain2_hash"}
  ]
}
```

#### Fields

| Field | Type | Description |
|-------|------|-------------|
| blocktemplate_blob | string | Updated block with merkle root embedded |
| blockhashing_blob | string | Block header for mining (140 bytes) |
| merkle_root | string | Merkle root of aux PoW hashes |
| merkle_tree_depth | numeric | Encoded as (nonce \| (depth << 16)) |
| aux_pow | array | Aux chains in slot order |

#### Merge Mining Workflow

```python
# 1. Get block templates from all chains
juno_template = juno_rpc.getblocktemplate()
chain2_template = chain2_rpc.getblocktemplate()

# 2. Prepare aux_pow data
aux_pow = [
    {
        "id": hashlib.sha256(b"chain2").hexdigest(),
        "hash": chain2_template['previousblockhash']
    }
]

# 3. Add aux PoW to JunoCash block
result = juno_rpc.add_aux_pow({
    "blocktemplate_blob": juno_template_hex,
    "aux_pow": aux_pow
})

# 4. Mine the updated block
mine_block(result['blockhashing_blob'])

# 5. When solution found, submit to both chains
if found_solution:
    juno_rpc.submitblock(result['blocktemplate_blob'])
    chain2_rpc.submitblock(chain2_block_with_proof)
```

## RandomX Considerations

### Seed Hash System

JunoCash uses epoch-based RandomX seeds similar to Monero:

- **Epoch Duration**: 2048 blocks
- **Seed Height Calculation**: `(block_height / 2048) * 2048`
- **Genesis Epoch**: Blocks 0-2047 use seed `0x0800000000...`

The RandomX dataset is generated from the block hash at the seed height and remains valid for the entire epoch.

### Performance Optimization

For p2pool implementations:

1. **Pre-cache next epoch**: Use `randomxnextseedhash` from `getblocktemplate` to prepare next dataset
2. **Fast mode**: Enable `-randomxfastmode=1` on daemon (2GB RAM vs 256MB)
3. **Persistent cache**: Keep daemon running to avoid dataset regeneration
4. **Batch hashing**: Submit multiple nonce attempts in parallel if using `calc_pow`

### Seed Transition

When crossing epoch boundaries:

```python
current_epoch = height // 2048
next_epoch = (height + 1) // 2048

if next_epoch != current_epoch:
    # Epoch transition - seed will change
    next_seed = get_block_hash(next_epoch * 2048)
    precache_randomx_dataset(next_seed)
```

## P2Pool Implementation Guide

### Architecture Overview

A typical JunoCash p2pool implementation consists of:

1. **Sidechain**: Decentralized mini-blockchain tracking shares
2. **Mainchain Monitor**: Polls `getminerdata` for blockchain state
3. **Share Validation**: Validates RandomX hashes via `calc_pow`
4. **Block Assembly**: Constructs mainchain blocks from sidechain
5. **Network Protocol**: P2P sharing of sidechain shares

### Minimal P2Pool Flow

```python
class JunoCashP2Pool:
    def __init__(self, rpc_url):
        self.rpc = BitcoinRPC(rpc_url)
        self.sidechain = SideChain()

    def run(self):
        while True:
            # 1. Get blockchain state
            miner_data = self.rpc.getminerdata()

            # 2. Update sidechain if new block
            if self.update_mainchain(miner_data):
                self.sidechain.new_block(miner_data['height'])

            # 3. Select transactions for next block
            txs = self.select_transactions(miner_data['tx_backlog'])

            # 4. Build sidechain share template
            share_template = self.sidechain.create_share_template(
                miner_address=self.address,
                transactions=txs
            )

            # 5. Mine sidechain shares
            self.mine_shares(share_template, miner_data)

            time.sleep(1)

    def mine_shares(self, template, miner_data):
        # Sidechain shares use same RandomX as mainchain
        # but with lower difficulty
        seed_hash = miner_data['randomxseedhash']

        for nonce in range(1000000):
            header = template.header(nonce)
            hash_hex = self.rpc.calc_pow(header.hex(), seed_hash)
            hash_int = int(hash_hex, 16)

            # Check sidechain share difficulty
            if hash_int < self.sidechain.share_target:
                share = template.finalize(nonce, hash_hex)
                self.sidechain.add_share(share)
                self.broadcast_share(share)

            # Check mainchain difficulty
            if hash_int < miner_data['target']:
                block = self.build_mainchain_block(template, nonce)
                self.rpc.submitblock(block.hex())
                print(f"BLOCK FOUND at height {miner_data['height']}!")
                break
```

### Transaction Selection

P2pool should prioritize transactions by fee rate:

```python
def select_transactions(tx_backlog, max_size=1000000):
    # Sort by fee per byte
    sorted_txs = sorted(
        tx_backlog,
        key=lambda tx: tx['fee'] / tx['size'],
        reverse=True
    )

    selected = []
    total_size = 0

    for tx in sorted_txs:
        if total_size + tx['size'] > max_size:
            break
        selected.append(tx['txid'])
        total_size += tx['size']

    return selected
```

### Payout Distribution

Sidechain shares determine payout distribution:

```python
def calculate_payouts(sidechain, block_reward):
    # Get last N shares from sidechain
    shares = sidechain.get_recent_shares(window=2016)

    # Calculate each miner's contribution
    contributions = {}
    for share in shares:
        miner = share.miner_address
        difficulty = share.difficulty
        contributions[miner] = contributions.get(miner, 0) + difficulty

    # Distribute block reward proportionally
    total_difficulty = sum(contributions.values())
    payouts = {}

    for miner, difficulty in contributions.items():
        share = difficulty / total_difficulty
        payouts[miner] = int(block_reward * share)

    return payouts
```

## Testing

### Manual RPC Testing

```bash
# Test getminerdata
junocash-cli getminerdata

# Test calc_pow with dummy data
HEADER_HEX="0400000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000"
SEED="0800000000000000000000000000000000000000000000000000000000000000"
junocash-cli calc_pow "$HEADER_HEX" "$SEED"

# Test add_aux_pow
TEMPLATE=$(junocash-cli getblocktemplate | jq -r '.blocktemplate')
junocash-cli add_aux_pow "{
  \"blocktemplate_blob\": \"$TEMPLATE\",
  \"aux_pow\": [{
    \"id\": \"1111111111111111111111111111111111111111111111111111111111111111\",
    \"hash\": \"2222222222222222222222222222222222222222222222222222222222222222\"
  }]
}"
```

### Performance Benchmarks

Expected performance on modern hardware:

| Operation | Time | Notes |
|-----------|------|-------|
| getminerdata | <10ms | Lightweight query |
| calc_pow | 10-100ms | Depends on CPU, RandomX mode |
| add_aux_pow | <5ms | Pure computation, no I/O |
| submitblock | 50-500ms | Block validation + propagation |

## Security Considerations

### RandomX Cache Poisoning

- P2pool implementations using `calc_pow` trust the daemon's RandomX implementation
- Ensure daemon is local or connected via authenticated RPC
- Malicious daemon could return incorrect hashes

### Sidechain Attacks

- 51% attacks on sidechain don't affect mainchain security
- Sidechain difficulty should be tuned to ~5-10 second share time
- Monitor sidechain for deep reorgs and unusual hash rate spikes

### Merge Mining Integrity

- Aux PoW merkle root must match across all miners
- Coordinated nonce selection prevents slot collisions
- Validate merkle proofs when submitting to child chains

## Configuration

### Daemon Configuration

For optimal p2pool support, configure `junocash.conf`:

```conf
# Enable mining support
gen=0  # Don't auto-mine, p2pool will control this

# RPC configuration
rpcuser=p2pool
rpcpassword=your_secure_password_here
rpcallowip=127.0.0.1

# RandomX optimization
randomxfastmode=1  # Use 2GB fast mode
randomxmsr=1       # Enable MSR optimizations (requires root)

# Mempool configuration
maxmempool=300  # MB, keep larger mempool for tx selection
mempoolexpiry=72  # Hours, keep txs longer

# Network
maxconnections=125
```

### P2Pool Configuration

Example p2pool configuration:

```yaml
# p2pool-junocash.yaml
daemon:
  host: 127.0.0.1
  port: 8232  # Default JunoCash RPC port
  username: p2pool
  password: your_secure_password_here

sidechain:
  share_target: 0x0000ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
  window_size: 2016  # PPLNS window in shares

mining:
  address: jcash1your_address_here
  threads: 4  # For share validation

network:
  p2p_port: 37889
  peers: []  # Bootstrap peers
```

## Built-in Miner Integration

### Overview

JunoCash's built-in miner (`setgenerate`) is designed for solo mining but can be adapted for p2pool use. However, currently p2pool mining requires a separate mining client because:

1. **P2Pool builds custom coinbase transactions** for payout distribution
2. **Share submission** differs from block submission
3. **Sidechain synchronization** requires bidirectional communication

### Current Status: Miner Integration

Currently, users can use external mining software or custom scripts to connect to p2pool:

#### Option 1: XMRig (External Miner)

XMRig supports RandomX and can be configured for JunoCash p2pool:

```bash
# Install xmrig
git clone https://github.com/xmrig/xmrig.git
cd xmrig && mkdir build && cd build
cmake .. -DWITH_RANDOMX=ON
make -j$(nproc)

# Configure for p2pool
cat > config.json <<EOF
{
  "autosave": true,
  "cpu": true,
  "opencl": false,
  "cuda": false,
  "pools": [
    {
      "url": "127.0.0.1:37888",
      "user": "YOUR_JUNOCASH_ADDRESS",
      "pass": "x",
      "rig-id": "worker1",
      "keepalive": true,
      "tls": false
    }
  ],
  "randomx": {
    "init": -1,
    "mode": "auto",
    "1gb-pages": false,
    "rdmsr": true,
    "wrmsr": true,
    "cache_qos": false,
    "numa": true
  },
  "cpu": {
    "enabled": true,
    "huge-pages": true,
    "hw-aes": null,
    "priority": null,
    "asm": true,
    "max-threads-hint": 100
  }
}
EOF

# Start mining
./xmrig
```

#### Option 2: Custom Miner Using RPC

Build a simple miner using the p2pool RPC methods:

```python
#!/usr/bin/env python3
import requests
import time
import struct

class SimpleP2PoolMiner:
    def __init__(self, daemon_url, p2pool_url, address):
        self.daemon = daemon_url
        self.p2pool = p2pool_url
        self.address = address

    def rpc_call(self, url, method, params=[]):
        payload = {
            "jsonrpc": "1.0",
            "id": "miner",
            "method": method,
            "params": params
        }
        r = requests.post(url, json=payload)
        return r.json()['result']

    def mine(self):
        while True:
            # Get work from p2pool
            work = self.rpc_call(self.p2pool, 'get_share_template', [self.address])

            # Mine share
            header_hex = work['header']
            target = int(work['target'], 16)
            seed_hash = work['seed_hash']

            print(f"Mining share, target difficulty: {work['difficulty']}")

            for nonce in range(1000000):
                # Update nonce in header
                header = bytearray.fromhex(header_hex)
                struct.pack_into('<I', header, 108, nonce)  # Nonce at offset 108

                # Calculate PoW via daemon
                header_hex_nonce = header.hex()
                pow_hash = self.rpc_call(
                    self.daemon,
                    'calc_pow',
                    [header_hex_nonce, seed_hash]
                )

                # Check if meets share target
                if int(pow_hash, 16) < target:
                    # Submit share to p2pool
                    self.rpc_call(
                        self.p2pool,
                        'submit_share',
                        [header_hex_nonce, self.address]
                    )
                    print(f"✓ Share found! Hash: {pow_hash[:16]}...")
                    break

            time.sleep(1)

# Usage
miner = SimpleP2PoolMiner(
    daemon_url='http://127.0.0.1:8232',
    p2pool_url='http://127.0.0.1:37889',
    address='jcash1your_address'
)
miner.mine()
```

### Future: Native Built-in Miner Support

To enable the built-in miner for p2pool, these enhancements would be needed:

#### 1. P2Pool Template Fetching

Add `-p2poolurl` option to fetch templates from p2pool:

```conf
# junocash.conf
p2poolurl=http://127.0.0.1:37889
p2pooladdress=jcash1your_mining_address
```

When enabled, the miner would:
- Poll p2pool's `get_share_template` RPC instead of creating local templates
- Submit shares via p2pool's `submit_share` RPC
- Automatically handle sidechain synchronization

#### 2. Share vs Block Submission

The miner would need to distinguish:

```cpp
// In BitcoinMiner() function
if (fP2PoolMode) {
    if (hash < sidechain_target) {
        // Submit share to p2pool
        SubmitShareToP2Pool(pblock, nonce);
    }
    if (hash < mainchain_target) {
        // P2pool will submit mainchain block
        // Miner just reports finding mainchain solution
        NotifyP2PoolMainchainBlock(pblock);
    }
} else {
    // Solo mining - submit directly to blockchain
    if (hash < mainchain_target) {
        ProcessNewBlock(state, chainparams, NULL, pblock, true, NULL);
    }
}
```

#### 3. Configuration Example

Once implemented, users could mine to p2pool as easily as solo mining:

```bash
# Solo mining (current)
junocashd -gen=1 -genproclimit=4 -mineraddress=jcash1your_address

# P2Pool mining (future)
junocashd -gen=1 -genproclimit=4 -p2poolurl=http://localhost:37889 -p2pooladdress=jcash1your_address
```

### Implementation Roadmap

1. **Phase 1** ✅ (Completed)
   - Implement core p2pool RPC methods
   - getminerdata, calc_pow, add_aux_pow
   - Documentation and testing

2. **Phase 2** (Community/Future)
   - Build p2pool daemon (separate project)
   - Implement sidechain consensus
   - Add share validation and payout logic
   - Create stratum server for external miners

3. **Phase 3** (Future Enhancement)
   - Integrate built-in miner with p2pool
   - Add `-p2poolurl` configuration option
   - Implement share submission interface
   - Add p2pool status monitoring to GUI

### Miner Considerations

Users can choose between external miners (like XMRig) or custom implementations:

**External Miners (XMRig):**
- **Hardware Optimization**: NUMA support, huge pages, MSR tuning for maximum hashrate
- **Flexibility**: Can mine to multiple pools/failovers
- **Monitoring**: Built-in benchmarking and statistics
- **Network Efficiency**: Persistent connections, optimized protocols

**Native / Custom Miners:**
- **Simplicity**: No external dependencies
- **Integration**: Direct interaction with the daemon
- **Control**: Full customization via RPC methods
- **Network Health**: Helps propagate the blockchain by running a full node


The built-in miner (when fully integrated) will offer the simplest user experience for p2pool mining.

## Future Enhancements

Potential improvements to JunoCash's p2pool support:

1. **Stratum Protocol**: Add native stratum server for external miners
2. **Share Templates**: RPC method to build sidechain share templates
3. **Batch calc_pow**: Accept multiple headers for parallel validation
4. **WebSocket Updates**: Push notifications for new blocks instead of polling
5. **Merge Mining Registry**: On-chain registration of child chains

## References

- [Monero P2Pool](https://github.com/SChernykh/p2pool) - Reference implementation
- [BIP 22](https://github.com/bitcoin/bips/blob/master/bip-0022.mediawiki) - getblocktemplate specification
- [RandomX](https://github.com/tevador/RandomX) - Proof-of-work algorithm
- [Merged Mining Specification](https://en.bitcoin.it/wiki/Merged_mining_specification)

## Support

For p2pool development support:

- GitHub Issues: https://github.com/junocash/junocash/issues
- Discord: #p2pool-dev channel
- Documentation: https://docs.junocash.org/p2pool

---

**Note**: P2Pool is experimental software. Test thoroughly on testnet before deploying to mainnet. Mining to p2pool does not provide the same level of decentralization as solo mining, but significantly improves over traditional centralized pools.
