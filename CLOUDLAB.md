# CloudLab Caracal/Felis fork

This branch preserves the source changes used for the three-node CloudLab
Caracal experiments. It is based on upstream Felis commit
`4a1e8d090fb636e4b061f3ef69a3f899e5605087`.

## Included changes

- Use ceiling division when creating NUMA slabs, allowing an 8-core d710 to
  initialize one slab instead of calculating zero NUMA nodes.
- Build a non-PIE executable for distributed execution. Caracal transmits
  routine addresses between processes, so PIE plus ASLR can make an address
  generated on one node invalid on another.
- Range-partition the YCSB table across all database nodes and route index
  lookups and transaction pieces to the owning node.
- Add `-XYcsbTableSize` so the YCSB dataset size can be selected at runtime.
- Pin Masstree to recoverable commit
  `8cec8f28a534a8d6ba1e1ec7399294d561190263`. The upstream gitlink referenced
  an object that is no longer available from the remote.

## External controller

The controller remains a separate repository and was not modified:

- Repository: <https://github.com/uoft-felis/felis-controller>
- Tested commit: `ec7bbfa862241b67cb39d6696dd78f057cd0766d`

Create its `config.json` from `cloudlab/config.example.json`, replacing every
placeholder with the addresses assigned to the current CloudLab experiment.
Do not reuse addresses from an expired reservation.

## Build

On every database node:

```bash
./cloudlab/build-nopie.sh debug
```

The successful distributed diagnosis and benchmark used the symbolized
non-PIE executable `debug/db`. A release build can be produced with:

```bash
./cloudlab/build-nopie.sh release
```

The build requires Clang, libc++, LLD, SCons, huge pages, and the repository
submodules. Reserve sufficient huge pages and run the database with an
unlimited locked-memory limit.

## Three-node TPC-C configuration

Use one controller and three d710 database nodes. Each database process uses
eight worker cores, a 4 GB generic-memory slab, three warehouses, and a two
million-entry execution queue.

```bash
sudo prlimit --memlock=unlimited:unlimited -- \
  ./debug/db -c CONTROLLER_IP:3148 -n host1 -w tpcc \
  -Xcpu8 -Xmem4G -XMaxNodeLimit3 -XTpccWarehouses3 \
  -XEpochQueueLength2M -XEpochSize33333 -XNrEpoch2
```

Run the same command on `host2` and `host3`, changing only `-n`.

TPC-C scales used by the distributed experiment:

| Nominal scale | Epoch size per node | `NrEpoch` | Exact cluster transactions |
| ---: | ---: | ---: | ---: |
| 100K | 33,333 | 2 | 99,999 |
| 1M | 33,333 | 11 | 999,990 |

Epoch zero is reserved. Exact transactions equal
`(NrEpoch - 1) * EpochSize * 3`.

## Three-node YCSB extension

This is a project extension, not upstream native YCSB. Each transaction
accesses ten keys.

```bash
sudo prlimit --memlock=unlimited:unlimited -- \
  ./debug/db -c CONTROLLER_IP:3148 -n host1 -w ycsb \
  -Xcpu8 -Xmem4G -XMaxNodeLimit3 -XEpochQueueLength2M \
  -XYcsbTableSize102400 -XEpochSize33333 -XNrEpoch2
```

| Nominal scale | Records | Epoch size per node | `NrEpoch` | Exact transactions |
| ---: | ---: | ---: | ---: | ---: |
| 100K | 102,400 | 33,333 | 2 | 99,999 |
| 1M | 1,048,576 | 10,000 | 34 | 990,000 |

The smaller 1M epoch size avoids the scheduler pending-queue limit observed
with 33,333 transactions per node per epoch.

## Start and stop

After every database node reports that it is ready, tell the controller to
connect the nodes and begin execution:

```bash
curl -X POST http://CONTROLLER_IP:8666/broadcast/ \
  -d '{"type":"status_change","status":"connecting"}'
```

After collecting results:

```bash
curl -X POST http://CONTROLLER_IP:8666/broadcast/ \
  -d '{"type":"status_change","status":"exiting"}'
```

## Measurement limits

The timed path is in-memory deterministic execution. It excludes initial data
loading and transaction generation and does not include consensus, WAL,
`fsync`, durable commit, or recovery. Reported latency is epoch/batch latency,
not client-observed per-transaction latency. SmallBank is not implemented in
this revision, and no valid distributed 4M result was produced on d710.
