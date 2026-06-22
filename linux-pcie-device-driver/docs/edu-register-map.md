# QEMU edu Register Map

This driver uses the register map from QEMU's `docs/specs/edu.txt` and
`hw/misc/edu.c` as the source of truth; the [README](../README.md#architecture)
diagram matches the table below. This document goes one level deeper, covering
DMA addressing convention and the 64-bit register write order.

BAR0 is a 1 MiB MMIO region. The device has an internal DMA buffer at
`DMA_START = 0x40000` with size `DMA_SIZE = 4096` bytes. QEMU's edu device uses
a 28-bit DMA mask, so the driver must program DMA addresses that are reachable
inside the low 256 MiB of guest physical address space.

| Offset | Name | Width | Access | Semantics |
| --- | --- | --- | --- | --- |
| `0x00` | `REG_IDENT` | 32 | RO | `0xRRrr00ed`; driver checks low 16 bits for magic `0x00ed`. |
| `0x04` | `REG_LIVENESS` | 32 | RW | Write `X`, read back `~X`. |
| `0x08` | `REG_FACTORIAL` | 32 | RW | Write `N` to start async factorial; result overwrites the register on completion. |
| `0x20` | `REG_STATUS` | 32 | bit0 RO, bit7 RW | bit0 is `STATUS_COMPUTING`; bit7 is `STATUS_IRQFACT` and must be set by the driver to enable factorial-done interrupts. |
| `0x24` | `REG_IRQ_STATUS` | 32 | RO | Pending interrupt bitmask: factorial done is `0x01`, DMA done is `0x100`. |
| `0x60` | `REG_IRQ_RAISE` | 32 | WO | Software interrupt raise; unused by this driver. |
| `0x64` | `REG_IRQ_ACK` | 32 | WO | Write-1-to-clear interrupt bits from `REG_IRQ_STATUS`. |
| `0x80` | `REG_DMA_SRC` | 64 | RW | DMA source address. |
| `0x88` | `REG_DMA_DST` | 64 | RW | DMA destination address. |
| `0x90` | `REG_DMA_CNT` | 64 | RW | Transfer length in bytes, up to 4096. |
| `0x98` | `REG_DMA_CMD` | 32 | RW | bit0 starts DMA, bit1 selects direction, bit2 requests DMA completion interrupt. |

## DMA addressing convention

When programming DMA registers, addresses are as follows:

- **Host → device** (`DMA_DIR=0`): `SRC` = host physical DMA address (`dma_handle`),
  `DST` = `0x40000` (`DMA_START`, the device-internal buffer address).
- **Device → host** (`DMA_DIR=1`): `SRC` = `0x40000`, `DST` = host physical DMA address.

The `DMA_START = 0x40000` value is an absolute device-side address (not relative).
This is confirmed by `edu_dma_timer` in `hw/misc/edu.c`, which compares the programmed
address directly against `DMA_START`.

All 64-bit DMA registers (`REG_DMA_SRC`, `REG_DMA_DST`, `REG_DMA_CNT`) are written
as two sequential 32-bit MMIO writes — low word first, then high word — using the
`edu_write64()` helper in `pcie_edu.c`.

