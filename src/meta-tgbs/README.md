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

### Shared static root filesystem

The demo image inherits `tgbs-rootfs`, enabling `read-only-rootfs` and generating
`/etc/tgbs-rootfs-id` from the base's file contents and metadata. Both targets
use a WIC disk: QEMU has a root partition and an ext4 overlay partition, while
Zybo also has a boot partition. The overlay partition is labelled
`tgbs-state`; `TGBS_STATE_SIZE` controls its initial size in MiB (default: 16
on Zybo, 256 on QEMU). On Zybo, `tools/upload-sdcard.sh` expands partition 3
and its ext4 filesystem to fill the remaining SD-card space after flashing.
The generated WIC therefore only needs a small empty store. Flashing it with
another tool leaves that initial size until the partition and filesystem are
expanded separately. QEMU keeps its allocated capacity between launches.

Boot uses `init=/sbin/tgbs-preinit`. Before the real init, the pre-init mounts
the overlay partition at `/var/lib/tgbs`, creates the system overlay, and
retains a non-recursive read-only bind of the static image at
`/run/tgbs/rootfs/base`. A private tmpfs `/run` is installed before init and
kept by SysV. Neither the shared base nor the data store contains the temporary
staging mounts after the pivot.

```text
/run/tgbs/rootfs/base/              read-only shared base
/var/lib/tgbs/                     writable ext4 data partition
├── rootfs-id                      identity of the associated base
└── overlays/
    ├── system/
    │   ├── upper/
    │   └── work/
    └── containers/
        └── NAME/
            ├── upper/
            ├── work/
            └── lock
```

The host can inspect every upper through this store. Containers see only their
own merged filesystem: the base has an empty store mountpoint and their private
`/run` does not expose the host base or runtime markers. Host changes are not
inherited by containers. Build shared configuration into the image.

By default, a domain name identifies persistent storage. Stopping the domain
removes its cgroup and runtime marker, but retains its upper and work directory.
An exclusive storage lock is held by the supervisor through cleanup; OverlayFS
also rejects active upper/work sharing. `tgbsctl run --ephemeral ...` uses a
private tmpfs upper without changing the stored persistent upper. The domain
names `channels` and `rootfs` are reserved.

The system upper persists across reboots, as do container files outside volatile
mounts. `/run`, `/tmp`, and `/var/volatile` (including `/var/log`) remain
volatile on both the host and containers. Dropbear's key under
`/var/lib/dropbear` is persistent on the host.

Preserve a disk file to reuse its state between QEMU launches. Do not use
QEMU's snapshot mode to validate persistence across separate launches.

A missing/unmountable data volume stops boot before init. The default device
is `LABEL=tgbs-state`; `tgbs.state=/dev/...` can override it on the kernel
command line. No device is formatted at boot. Persistent domain startup refuses
a missing, non-ext4, read-only or mismatched store rather than writing into the
host overlay. Do not change a base or an upper directly while its overlay is
mounted. There is no runtime storage reset command in this version; back up and
reset storage only with the corresponding overlay stopped (the system upper
requires an offline reset).

The pre-init adjusts `/etc/fstab` and `/etc/default/rcS` in the system upper so
SysV treats its root as writable and does not fsck the overlay. The original
configuration remains in the static image. Other images using this runtime
should inherit `tgbs-rootfs`, provide the data partition and select the pre-init.
This boot flow expects a directly mounted disk root filesystem.

#### Base identity and image updates

The `tgbs-rootfs` image class runs `tgbs_rootfs_id` during rootfs
post-processing. This function writes a SHA-256 identifier to
`/etc/tgbs-rootfs-id`, using the base's paths, file contents, types, ownership,
permissions, symlink targets and device numbers. Modification times and the
identifier file itself are excluded. This identifies the built base; it is
separate from the partition UUID and is not an integrity check recalculated
at boot. Changes made through the system or container uppers do not change it.

At first boot, `tgbs-preinit` records that identifier in
`/var/lib/tgbs/rootfs-id` before creating the overlays. This associates all
system and container uppers on the data partition with one base. On subsequent
boots, the pre-init compares the stored identifier with the image's identifier
before mounting the system overlay. A mismatch stops boot before the real init
with `persistent overlays belong to another rootfs image`. `tgbsctl` also
checks this association before starting a persistent container. An existing
overlay store without an identifier is refused rather than adopted silently.

The check prevents old uppers from being reused automatically with a changed
base: a copied-up library can hide an updated library, and a whiteout can hide
a file introduced by the new image. There is no automatic migration or
deletion of old uppers. Updating the base while keeping the data partition
therefore requires an explicit migration or an offline reset of its overlays.
Changing only the stored identifier bypasses the check without making the old
uppers compatible. Replacing or reflashing the entire generated WIC image
provides a fresh empty data partition and resets both system and container state.

On a running target, inspect the two identifiers with:

```sh
cat /run/tgbs/rootfs/base/etc/tgbs-rootfs-id
cat /var/lib/tgbs/rootfs-id
```

Use the identifier from the shared base: `/etc/tgbs-rootfs-id` in the running
system is visible through the system overlay and could have been overwritten
in its upper.

#### Checking persistence

To check persistence on the target, create a host file, then enter `self`:

```sh
echo host > /home/root/host-test
tgbs-demo-self start
```

Inside `self`, the host file is absent. Create a separate container file:

```sh
test ! -e /home/root/host-test
echo container > /home/root/container-test
exit
```

Starting `self` again restores `container-test`. Rebooting restores both files
in their respective roots. The host can inspect the container's stored file at
`/var/lib/tgbs/overlays/containers/self/upper/home/root/container-test`;
`/run/tgbs/rootfs/base/home/root/host-test` remains absent. For a temporary
domain, add `--ephemeral` to a `tgbsctl run` command; it starts from the base
and discards its changes at exit without resetting the persistent upper.

### Domain startup

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
namespaces. The static image at `/run/tgbs/rootfs/base` is the lower layer of
an overlayfs whose writable upper is persistent by default. Startup fails if
the base is unavailable or writable. Applications see
a writable root, but their changes stay in their own upper and cannot overwrite regular
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
read-only; for a destination, the endpoint and `destination.lock` are writable
while `source.lock` is read-only. Both views refer to the same underlying
AF_UNIX socket directory. A domain may be both the source and destination of a
channel; in that case both role-specific views are writable, allowing a local
loopback channel without changing the application interface. For sampling,
the source owns the writable endpoint and destinations see it read-only;
`source.lock` is writable only for the source. All sampling participants share
the same directory containing the latest published message.

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
    ├── destination.lock
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

if (tgbs_queuing_channel_receive(rx, message, sizeof(message), &length,
                               100000) == -1) /* wait up to 100 ms */
        /* handle errno */;

tgbs_queuing_channel_close(rx);
```

The source takes the exclusive `source.lock` and uses an unnamed AF_UNIX
`SOCK_DGRAM` socket. The destination takes the exclusive `destination.lock`
and binds `channel.sock`; a second open for the same direction fails with
`EADDRINUSE`. A stale socket left by a crashed destination is removed only
after acquiring `destination.lock`. Open only prepares the local endpoint; the
source may open before the destination, provided the channel contract exists.
Each send attempt associates the source socket with `channel.sock`, so
`POLLOUT` can account for the destination queue as well as the sender buffer.
This does not turn the datagram channel into a stream. Association is refreshed
at every attempt, without a shared userspace connection flag; retrying send
after destination recreation does not require reopening the source. A pending
send may select the new destination when it retries.

Each successful send is one complete, nonempty message. The library caches and
enforces `max_message_size` when the handle is opened. A receive buffer that
is too small consumes the datagram and returns `EMSGSIZE` together with its
original size. The underlying file descriptor is available through
`tgbs_queuing_channel_fd()` for use with `poll()`/`epoll()`.

Send and receive take an `int64_t timeout_us` argument. Zero means no waiting
(`EAGAIN` if the queue is full or empty); a positive value means passive waiting
for that many microseconds (`ETIMEDOUT` on expiration); `TGBS_TIMEOUT_INFINITE`
means passive waiting without a time limit. Other negative values and durations
not representable by the platform wait timeout return `EINVAL`.
An absent, closed or disconnected destination returns `ENOTCONN` from send
immediately, including with an infinite timeout. No message is retained for
later delivery. Native unavailable-peer errors are normalized to `ENOTCONN`;
permission and resource errors are preserved. The demo opens both local
endpoints directly and retries send on `ENOTCONN` until the receiver is ready.
It explicitly uses infinite waits for queue space/data availability.

The backend first attempts I/O with `MSG_DONTWAIT`; on `EAGAIN`, it waits
passively in `ppoll()` for `POLLIN`/`POLLOUT`, unless the timeout is zero.
Signal interruptions and readiness races with other callers are retried using
the remaining duration from a single per-call `CLOCK_MONOTONIC` deadline.
Timer granularity and scheduling (including TGBS windows) can delay the actual
return beyond the requested duration. Concurrent calls on a shared handle,
including across `fork()`, have independent deadlines; they compete for
messages/space with no fairness guarantee. Socket timeout options and
`O_NONBLOCK` are not modified by I/O. Callers must not close the handle or
reconnect or shut down its socket while an operation is in progress.
Sampling API and behavior are unchanged.

AF_UNIX does not provide a per-socket limit or an exact status counter in
messages. Queuing contracts do not include `max_nb_message`.
This does not mean unlimited capacity: Linux still applies its internal socket
buffer and AF_UNIX datagram queue limits, so send can also need a timeout.
`tgbs_queuing_channel_get_status()` reports whether a message is pending and
the size of the next message.

For queuing, the endpoint locks enforce one `libtgbscomm` source and one
destination. Mount namespaces hide unrelated channels and expose only the lock corresponding to
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
period and whether the data endpoint is present. Creation and deletion check
every participant's cgroup. Sampling creates the contract, `lifetime.lock`, and
`endpoint/source.lock`. The `endpoint/sample` file appears on the first
publication; there is no destination lock or socket.

The sampling API uses an opaque `tgbs_sampling_channel_t`, with
`tgbs_sampling_channel_open()`, `write()`, `read()`, and `close()` functions
(all prefixed `tgbs_sampling_channel_`). Processes may open the same source or
destination independently, or use handles inherited through `fork()`. Each
handle holds a shared lifetime lock and uses close-on-exec descriptors.

The sample file contains an unsigned 64-bit little-endian `CLOCK_MONOTONIC`
timestamp in nanoseconds, followed by the message bytes. The message length
is the file size minus the 8-byte timestamp. The maximum size in the contract
applies to the message alone. The clock is shared by the TGBS domains and is
unaffected by changes to the system's wall-clock time.

Each write takes `source.lock` for the duration of publication, writes a unique
temporary file in the endpoint directory, records the timestamp after copying
the payload, and atomically replaces `sample` with `renameat()`. The published
file is read-only and is never modified in place by the library. Concurrent
source processes or threads are serialized; readers do not take a lock. A
reader opens `sample` for each call and obtains both its size and contents
from that descriptor, so it sees a complete version even during publication.
Reads do not consume the message or refresh its timestamp.

A read succeeds with `TGBS_SAMPLING_VALID` when the message age is strictly
less than `refresh_period_us`. An expired message remains readable, with
`TGBS_SAMPLING_INVALID`. Before the first publication, reads return `ENODATA`.
A buffer that is too small returns `EMSGSIZE` and the required message length;
the stored value is unchanged. Invalid file sizes or timestamps return
`EPROTO`. Read on a source or write on a destination returns `EINVAL`.

```c
#include <tgbs/sampling.h>

tgbs_sampling_channel_t *rx;
unsigned char message[256];
size_t length;
tgbs_sampling_validity_t validity;

if (tgbs_sampling_channel_open("position", TGBS_CHANNEL_DESTINATION, &rx) == -1)
        /* handle errno */;

if (tgbs_sampling_channel_read(rx, message, sizeof(message), &length, &validity) == -1)
        /* handle errno */;
else if (validity == TGBS_SAMPLING_INVALID)
        /* handle expired data */;

tgbs_sampling_channel_close(rx);
```

Closing a handle does not remove the last value, including when the producer
exits. The value expires according to its timestamp. Failed writes before
publication preserve the previous value. A process killed while preparing a
publication may leave a `.sample.tmp-` file; deleting an unused channel removes
its sample and reserved temporary files. Unrelated endpoint files are not
removed. Channel data lives under `/run` and is volatile across reboots.

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
