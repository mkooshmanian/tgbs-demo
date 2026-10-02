# meta-tgbs

Reusable Yocto/OpenEmbedded integration layer for TGBS.

## Machines

* `qemuarm32`: QEMU ARM `virt` with PL011 and built-in VirtIO devices.
* `zybo-z7`: Digilent Zybo Z7 (Zynq-7000), using the upstream
  `xilinx/zynq-zybo-z7.dtb` and U-Boot's `xilinx_zynq_virt_defconfig`.

Both kernel configurations start from a machine-specific minimal defconfig;
common TGBS, PREEMPT_RT and optional debug fragments are merged afterward, then
`olddefconfig` fills unspecified symbols from their Kconfig defaults.
Machine defconfigs live in `recipes-kernel/linux/files/machines/`, while
reusable feature fragments live in `recipes-kernel/linux/files/fragments/`.
Programmable-logic IP for the Zybo Z7 should be enabled in a separate kernel
fragment and described by a matching device-tree overlay.

## Runtime control

The image integration mounts a unified cgroup v2 hierarchy and enables the
`cpu`, `cpuset`, `memory`, and `pids` controllers. `tgbsctl` creates one direct
child of the cgroup root per managed workload and can configure its temporal
contract, CPU placement, resource limits, and optional DEADLINE bandwidth
reclaim:

```sh
tgbsctl run --name worker --hostname worker --runtime-us 20000 --period-us 100000 \
    --cpus 0 --reclaim false --memory-max 268435456 --pids-max 128 \
    /usr/bin/worker

tgbsctl set worker cpus 0-1
tgbsctl set worker cpus inherit
tgbsctl set worker reclaim true
tgbsctl set worker memory-max 536870912
tgbsctl set worker pids-max max
```

`run` uses `clone3(CLONE_INTO_CGROUP)` to create the command directly in its
configured TGBS cgroup and as PID 1 in new PID, mount, UTS, IPC, and cgroup
namespaces. The host root filesystem is the read-only lower layer of an
overlayfs whose writable upper layer lives in a private tmpfs. Applications see
a writable root, but their changes are ephemeral and cannot overwrite regular
host filesystem paths. Mount propagation is private; `/proc`, `/tmp`, and
`/run` are private mounts, `/sys` is read-only, and `/dev` keeps the host device
view for prototype compatibility. The cgroup namespace makes the domain appear
as `/` in `/proc/self/cgroup` and provides a read-only, domain-rooted cgroup2
view on `/sys/fs/cgroup`.
After preparing these privileged resources and optionally setting the hostname,
`tgbsctl` removes `CAP_SYS_ADMIN` from the command's capability sets and
bounding set, then enables `no_new_privs` before `exec`.

The private `/run/tgbs/channels` contains only channels whose source or one of
the destinations matches the domain. Contracts and lifetime locks are read-only.
For a queuing source, `source.lock` is writable while the endpoint directory is
read-only; for a destination, the endpoint and `receiver.lock` are writable
while `source.lock` is read-only. Both views refer to the same underlying
AF_UNIX socket directory. A domain may be both the source and destination of a
channel; in that case both role-specific views are writable, allowing a local
loopback channel without changing the application interface. For sampling,
the source owns the writable endpoint and destinations see it read-only;
`source.lock` is writable only for the source. Sampling data operations are
unsupported and return `ENOTSUP`.

CPU placement uses the standard `cpuset.cpus` CPU-list syntax. `inherit`
restores the cgroup root's current effective CPU list; this works for populated
domains, for which the kernel may reject an empty `cpuset.cpus`. Boolean reclaim
values accept `0`, `1`, `false`, and `true`.

`memory-max` is expressed in bytes and `pids-max` as a task count. Both
accept a strictly positive integer or `max`, which is also the default when the
corresponding `run` option is omitted. `inspect` reports their limits and
current usage, together with the peak memory usage.

`run --cpus` applies the CPU placement before the non-zero temporal contract,
so the kernel admission test sees the intended CPUs. With TGBS 1.4, a later
`set NAME cpus ...` updates the active per-CPU servers but does not rerun the
bandwidth admission test transactionally. Dynamic placement changes must
therefore keep the sum of overlapping reservations within the configured
DEADLINE bandwidth limit.

## Communication channels

`tgbsctl` manages immutable, unidirectional channel contracts independently
from TGBS domains:

```sh
tgbsctl channel create \
    --type queuing \
    --name command \
    --source producer \
    --destination consumer \
    --max-message-size 256

tgbsctl channel list
tgbsctl channel inspect command
tgbsctl channel delete command
```

Channels must be created before their participating domains are started. Each
creation gets a unique generation, so deleting and recreating the same name
still produces a distinct channel instance. The queuing runtime layout is:

```text
/run/tgbs/channels/NAME/
├── contract                 # root-owned, mode 0444
├── lifetime.lock
└── endpoint/
    ├── source.lock
    ├── receiver.lock
    └── channel.sock         # present while the destination is open
```

The contract records the type, name, generation, source, destination(s), and
`max_message_size`. `--type` is required and accepts `queuing` or `sampling`.
There is no in-place update command: changing these values requires deleting
and recreating the channel. The channel directory is mode `0555`, but this is not intended to
prevent the trusted host administrator from changing it manually.

Every library handle holds a shared `lifetime.lock`; deletion requires its
exclusive lock and is therefore refused while a handle is open. Deletion is
also refused while any participant cgroup is populated. Creation, deletion,
and participant startup share a topology lock so that these checks cannot race.

`libtgbscomm` provides separate APIs in `<tgbs/queuing.h>` and
`<tgbs/sampling.h>`. Both use the common direction constants from
`<tgbs/types.h>`. The queuing data path is:

```c
#include <tgbs/queuing.h>

tgbs_queuing_channel_t *rx;
unsigned char message[256];
size_t length;

if (tgbs_queuing_channel_open("command", TGBS_CHANNEL_DESTINATION, &rx) == -1)
        /* handle errno */;

if (tgbs_queuing_channel_receive(rx, message, sizeof(message), &length) == -1)
        /* handle errno */;

tgbs_queuing_channel_close(rx);
```

The source takes the exclusive `source.lock` and uses an unnamed AF_UNIX
`SOCK_DGRAM` socket. The destination takes the exclusive `receiver.lock`
and binds `channel.sock`; a second open for the same direction fails with
`EADDRINUSE`. A stale socket left by a crashed destination is removed only
after acquiring `receiver.lock`. The source may open before the destination,
but sends fail until `channel.sock` exists.

Each successful send is one complete, nonempty message. The library caches and
enforces `max_message_size` when the handle is opened. A receive buffer that
is too small consumes the datagram and returns `EMSGSIZE` together with its
original size. The underlying file descriptor is available through
`tgbs_queuing_channel_fd()` for use with `poll()`/`epoll()`.

AF_UNIX does not provide a per-socket limit or an exact status counter in
messages. Queuing contracts do not include `max_nb_message`.
`tgbs_queuing_channel_get_status()` reports whether a message is pending and
the size of the next message.

The endpoint locks enforce one `libtgbscomm` source and one destination. Mount
namespaces hide unrelated channels and expose only the lock corresponding to
the domain's role as writable. Container processes cannot alter this view
because `CAP_SYS_ADMIN` is removed before the application starts. This does not
form a complete security boundary: the processes use the host root identity
and retain the host device view. The overlay is intended to prevent
accidental host filesystem modification, not to contain a malicious workload.
No credential passing or `SO_PASSCRED` is used.

The locks prevent independent opens for the same role, rather than preventing
multiple processes from using a handle inherited through `fork()`. Such
processes share the socket and its queue. Descriptors are close-on-exec, and
closing an inherited destination handle unlinks the socket path.

Opening a channel through the wrong typed API returns `EPROTOTYPE`.

### Sampling channels

```sh
tgbsctl channel create \
    --type sampling \
    --name position \
    --source producer \
    --destination display \
    --destination recorder \
    --max-message-size 256 \
    --refresh-period-us 100000
```

Sampling accepts between 1 and 64 distinct destinations and requires a positive
refresh period in microseconds, common to all subscribers. The period must fit
in a 64-bit nanosecond duration. Queuing accepts exactly one destination and
rejects a refresh period. Both types support message sizes up to
`INT_MAX - 32` bytes.

`list` and `inspect` show the type, all destinations, and the sampling refresh
period. Creation and deletion check every participant's cgroup. Sampling
creates the contract, `lifetime.lock`, and `endpoint/source.lock`; it does not
create a receiver lock, socket, or sample file.

The sampling API declares an opaque `tgbs_sampling_channel_t`, with
`tgbs_sampling_channel_open()`, `write()`, `read()`, and `close()` functions
(all prefixed `tgbs_sampling_channel_`). Sampling supports contract management
and role-specific mounts. Opening a valid sampling contract returns `ENOTSUP`,
leaves the handle NULL, releases the lifetime lock, and creates no data
endpoint. Message publication, reading, and freshness evaluation are
unsupported.

## Runtime monitor

`tgbs-top` is a lightweight interactive CPU monitor limited to domains managed
by `tgbsctl` and to the tasks inside those domains:

```sh
tgbs-top
```

The first section shows every logical CPU and aggregates busy time into TGBS,
other, and idle shares. Domain rows show the configured per-CPU reservation
(`cpu.runtime_us / cpu.period_us`) and a `USE/BUDGET` bar. That bar compares
measured CPU consumption with the total reservation across the effective CPU
set; a full bar means that the domain consumed its complete reserved capacity
during the latest sample. Task rows show the scheduling policy, priority,
current CPU, CPU time consumed during the latest sample (`CPU ms`), and context
switches per second (`CSW/s`). CPU percentages use the same convention as
`top`: 100% is one fully occupied logical CPU.

Press `q` to leave the interactive view. For scripts or captures, use batch
mode, for example:

```sh
tgbs-top --batch --iterations 3 --delay 0.5
```
