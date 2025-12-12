# P2Pool Mining Mode Control Implementation Plan

## Overview

Add comprehensive P2Pool mining controls to junocashd with:
- Automatic P2Pool process management (start/stop/monitor/auto-restart)
- Dual control interfaces (RPC command + terminal UI keyboard shortcut 'O')
- Auto-start on daemon startup if previously enabled
- Status monitoring (mining mode, connection status, connected miners, process health)
- Seamless switching between solo and P2Pool modes

## User Requirements (Confirmed)

1. **Control UI**: Both terminal keyboard shortcut ('O') AND RPC command
2. **Auto-start**: P2Pool auto-starts on daemon startup if previously enabled
3. **Crash handling**: Auto-restart P2Pool with exponential backoff (max 5 attempts)
4. **Status display**: Show all of:
   - Mining mode (Solo/P2Pool)
   - P2Pool connection status
   - Connected miners count
   - P2Pool process health (PID, uptime)

## Architecture

### Component Design

```
┌─────────────────────────────────────────────────────────┐
│ junocashd                                               │
│                                                         │
│  ┌───────────────────────────────────────────┐         │
│  │ P2PoolProcessManager (Singleton)          │         │
│  │ - Start/Stop/Restart P2Pool binary        │         │
│  │ - Health monitoring (PID + HTTP checks)   │         │
│  │ - Auto-restart with exponential backoff   │         │
│  └───────────────────────────────────────────┘         │
│                      ↕                                  │
│  ┌───────────────────────────────────────────┐         │
│  │ P2PoolStatusMonitor (Singleton)           │         │
│  │ - Poll P2Pool HTTP API for status         │         │
│  │ - Cache results (5s TTL)                  │         │
│  │ - Return: miners count, shares, hashrate  │         │
│  └───────────────────────────────────────────┘         │
│                      ↕                                  │
│  ┌───────────────────────────────────────────┐         │
│  │ Control Interfaces                        │         │
│  │ - RPC: setp2poolmode(enable, autostart)  │         │
│  │ - Terminal UI: 'O' key toggle            │         │
│  │ - Updates mapArgs["-p2poolurl"]          │         │
│  │ - Restarts GenerateBitcoins()            │         │
│  └───────────────────────────────────────────┘         │
│                      ↕                                  │
│  ┌───────────────────────────────────────────┐         │
│  │ Auto-Start (init.cpp)                     │         │
│  │ - Check -p2poolautostart on startup       │         │
│  │ - Launch P2Pool if enabled                │         │
│  └───────────────────────────────────────────┘         │
└─────────────────────────────────────────────────────────┘
```

## Implementation Plan

### Phase 1: Process Management Module

**Create: src/p2pool_manager.h** (~210 lines)
- `P2PoolProcessManager` singleton class
- Methods: Start(), Stop(), Restart(), IsRunning(), GetPID(), GetUptime()
- `P2PoolConfig` struct with: binaryPath, walletAddress, host, rpcPort, lightMode
- Private: process spawning, monitoring thread, health checks

**Create: src/p2pool_manager.cpp** (~650 lines)
- **Process Spawning (Linux)**: fork() + execv(), redirect stdout/stderr to p2pool.log
- **Process Spawning (Windows)**: CreateProcess() with log file redirection
- **Health Monitoring Thread**: Every 5s check:
  - PID alive (kill(pid, 0) on Linux, OpenProcess() on Windows)
  - HTTP health check (http://127.0.0.1:37889/stats)
- **Auto-Restart Logic**: Exponential backoff (1s, 2s, 4s, 8s, 16s max), max 5 attempts
- **Graceful Shutdown**: SIGTERM, wait 5s, SIGKILL if needed

**Configuration Resolution**:
- Binary path: `-p2poolbinary` flag or default `<datadir>/junocash-p2pool`
- Wallet address: `-p2pooladdress` or `-mineraddress`
- RPC credentials: Use same as junocashd RPC

**P2Pool Launch Arguments**:
```
junocash-p2pool --host 127.0.0.1 --rpc-port 8232 \
  --wallet <address> --stratum 0.0.0.0:37889 \
  [--light-mode if -p2poollightmode set]
```

### Phase 2: Status Monitoring Module

**Create: src/p2pool_status.h** (~85 lines)
- `P2PoolStatus` struct: connected, connectedMiners, totalShares, poolHashrate
- `P2PoolStatusMonitor` singleton class
- Methods: GetStatus(), IsReady()

**Create: src/p2pool_status.cpp** (~280 lines)
- Poll P2Pool HTTP API endpoint: `http://127.0.0.1:37889/stats`
- Parse JSON response for: connections, shares_found, pool_hashrate
- 5-second cache to avoid excessive polling
- Return default values if P2Pool unavailable

### Phase 3: RPC Interface

**Modify: src/rpc/mining.cpp** (+200 lines)

**New RPC Command: `setp2poolmode`**
```cpp
setp2poolmode enable ( autostart )

Arguments:
1. enable     (boolean, required) true=enable P2Pool, false=disable
2. autostart  (boolean, optional, default=true) Persist for auto-start

Returns:
{
  "p2poolmode": true,
  "p2poolrunning": true,
  "pid": 12345
}

Errors:
- RPC_INVALID_PARAMETER: No wallet address configured
- RPC_INTERNAL_ERROR: P2Pool failed to start
```

**Implementation Logic**:
1. If enabling:
   - Validate wallet address configured
   - Start P2Pool via P2PoolProcessManager
   - Wait up to 10s for P2Pool ready
   - Set mapArgs["-p2poolurl"] = "http://127.0.0.1:37889"
   - Set mapArgs["-p2pooladdress"] = wallet_addr
   - If autostart: Set mapArgs["-p2poolautostart"] = "1"
   - Restart mining threads if currently mining

2. If disabling:
   - Clear mapArgs["-p2poolurl"] and ["-p2pooladdress"]
   - Set mapArgs["-p2poolautostart"] = "0"
   - Stop P2Pool process
   - Restart mining threads in solo mode if currently mining

**Extend `getmininginfo` RPC** (+15 lines):
Add fields:
- `p2poolmode`: true/false
- `p2poolrunning`: true/false
- `p2poolpid`: process ID
- `p2pooluptime`: seconds since start
- `p2poolconnected`: HTTP connectivity status
- `p2poolminers`: number of connected miners
- `p2poolshares`: total shares found

### Phase 4: Terminal UI Integration

**Modify: src/metrics.cpp** (~150 lines changes)

**Update printMiningStatus() function** (around line 1224):
```
┌─ MINING ─────────────────────────────┐
│ Status: ● ACTIVE - 4 threads         │
│ Mode: P2Pool Mining                  │  <-- NEW
│ P2Pool PID: 12345                    │  <-- NEW
│ P2Pool Uptime: 2h 15m                │  <-- NEW
│ P2Pool Status: ● Connected           │  <-- NEW
│ Connected Miners: 3                  │  <-- NEW
│ CPU: Intel Core i7...                │
│ Hashrate: 2300 H/s                   │
└──────────────────────────────────────┘
```

Color coding:
- Mode "P2Pool Mining": Cyan (\e[1;36m)
- Mode "Solo Mining": Default
- Status "Connected": Green (\e[1;32m)
- Status "Disconnected": Red (\e[1;31m)

**Add keyboard handler** (around line 3158):
```cpp
} else if (key == 'O' || key == 'o') {
    toggleP2PoolMode();
    break;
}
```

**Implement toggleP2PoolMode() function** (+80 lines):
- Check current mode via IsP2PoolModeActive()
- If enabling:
  - Prompt user if no wallet address configured
  - Start P2Pool process
  - Update mapArgs
  - Restart mining threads
- If disabling:
  - Stop P2Pool process
  - Clear mapArgs
  - Restart mining in solo mode
- Show status messages during transition

**Update help text** (line ~3110):
```
[Q: Quit] [M: Mining] [T: Threads] [O: P2Pool]
```

### Phase 5: Auto-Start Integration

**Modify: src/init.cpp** (+80 lines)

**In AppInit2()** (around line 2100, after mining initialization):
```cpp
#ifdef ENABLE_MINING
    if (GetBoolArg("-p2poolautostart", false)) {
        string addr = GetArg("-p2pooladdress", GetArg("-mineraddress", ""));
        if (!addr.empty()) {
            LogPrintf("Auto-starting P2Pool (address: %s)\n", addr);

            P2PoolConfig config;
            config.binaryPath = GetP2PoolBinaryPath();
            config.walletAddress = addr;
            config.host = "127.0.0.1";
            config.rpcPort = GetArg("-rpcport", 8232);

            P2PoolProcessManager& mgr = P2PoolProcessManager::GetInstance();
            if (mgr.Start(config)) {
                mapArgs["-p2poolurl"] = "http://127.0.0.1:37889";
                mapArgs["-p2pooladdress"] = addr;
            }
        }
    }
#endif
```

**In Shutdown()** (around line 300):
```cpp
P2PoolProcessManager& mgr = P2PoolProcessManager::GetInstance();
if (mgr.IsRunning()) {
    LogPrintf("Stopping P2Pool...\n");
    mgr.Stop();
}
```

**Add help text** (around line 500):
```cpp
strUsage += HelpMessageGroup(_("P2Pool options:"));
strUsage += HelpMessageOpt("-p2poolautostart", _("Auto-start P2Pool on daemon startup (default: false)"));
strUsage += HelpMessageOpt("-p2pooladdress=<addr>", _("Wallet address for P2Pool rewards"));
strUsage += HelpMessageOpt("-p2poolbinary=<path>", _("Path to P2Pool binary (default: <datadir>/junocash-p2pool)"));
strUsage += HelpMessageOpt("-p2poollightmode", _("Run P2Pool in light mode (saves 2GB RAM)"));
```

### Phase 6: Mining Mode Coordination

**Modify: src/miner.cpp** (+50 lines)

**Add helper function** (after line 1567):
```cpp
bool IsP2PoolModeActive() {
    return !GetArg("-p2poolurl", "").empty() &&
           !GetArg("-p2pooladdress", "").empty();
}
```

**Dynamic mode switching in BitcoinMiner()** (line 1150):
- Already checks `-p2poolurl` at start and redirects to P2PoolMiner()
- Current implementation is sufficient - mode changes require mining restart
- GenerateBitcoins() stops and restarts threads, picking up new flags

### Phase 7: Build System Integration

**Modify: src/Makefile.am** (+4 lines in libbitcoin_server_a_SOURCES):
```makefile
p2pool_manager.h \
p2pool_manager.cpp \
p2pool_status.h \
p2pool_status.cpp \
```

## Configuration Options

| Option | Type | Default | Description |
|--------|------|---------|-------------|
| `-p2poolautostart` | bool | false | Auto-start P2Pool on daemon startup |
| `-p2pooladdress=<addr>` | string | (empty) | Wallet address for P2Pool rewards |
| `-p2poolbinary=<path>` | string | `<datadir>/junocash-p2pool` | P2Pool binary path |
| `-p2poollightmode` | bool | false | Run P2Pool in light mode |

## Error Handling

| Error | Detection | Response |
|-------|-----------|----------|
| Binary not found | File existence check | Fail with clear error message |
| Process spawn fails | fork/CreateProcess return | Log error, return false |
| Port conflict | P2Pool stderr parsing | Log error suggesting port config |
| Process crash | PID check fails | Auto-restart with backoff (max 5) |
| HTTP timeout | Client exception | Increment failures, restart after 3 |
| Invalid address | Validation before start | Fail with validation error |

## Testing Strategy

1. **Unit Tests** (src/test/p2pool_tests.cpp):
   - Config validation
   - Process lifecycle
   - Auto-restart backoff
   - Mode switching

2. **RPC Tests** (qa/rpc-tests/p2pool.py):
   - setp2poolmode enable/disable
   - getmininginfo extended fields
   - Auto-start behavior
   - Error handling

3. **Integration Tests**:
   - Full workflow: enable → mine → restart daemon → verify auto-start
   - Crash recovery: kill P2Pool → verify auto-restart
   - Mode switching: toggle while mining → verify no crashes

## Critical Files

1. **src/p2pool_manager.cpp** (NEW) - Process lifecycle management core
2. **src/rpc/mining.cpp** - RPC command setp2poolmode() and getmininginfo() extension
3. **src/metrics.cpp** - Terminal UI status display and 'O' keyboard toggle
4. **src/init.cpp** - Auto-start on daemon launch and shutdown cleanup
5. **src/miner.cpp** - Mining mode helper IsP2PoolModeActive()

## Implementation Sequence

1. Phase 1: Process Management (~650 lines, 2 files)
2. Phase 2: Status Monitoring (~365 lines, 2 files)
3. Phase 3: RPC Interface (~215 lines, 1 file modified)
4. Phase 4: Terminal UI (~150 lines, 1 file modified)
5. Phase 5: Auto-Start (~80 lines, 1 file modified)
6. Phase 6: Mode Coordination (~50 lines, 1 file modified)
7. Phase 7: Build System (~4 lines, 1 file modified)

**Total**: ~1,514 lines across 4 new files + 5 modified files
