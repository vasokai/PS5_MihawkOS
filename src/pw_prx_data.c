/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pw_prx_data.h"

int pw_prx_data_move(const PwPrxDataOps *ops, uintptr_t address, size_t bytes, int protection)
{
    uint64_t offset = 0;
    uintptr_t scratch = 0;
    if (!ops || !ops->page || !bytes || (address % ops->page) || (bytes % ops->page) || (protection & 4))
        return PW_PRX_DATA_KEPT;
    if (!ops->allocate || !ops->map_new || !ops->map_fixed || !ops->map_anon || !ops->unmap || !ops->release ||
        !ops->copy)
        return PW_PRX_DATA_KEPT;
    if (ops->allocate(ops->context, bytes, &offset)) return PW_PRX_DATA_KEPT;
    if (ops->map_new(ops->context, bytes, offset, &scratch) || !scratch) {
        ops->release(ops->context, offset, bytes);
        return PW_PRX_DATA_KEPT;
    }
    ops->copy((void *)scratch, (const void *)address, bytes);
    if (ops->unmap(ops->context, address, bytes)) {
        ops->unmap(ops->context, scratch, bytes);
        ops->release(ops->context, offset, bytes);
        return PW_PRX_DATA_KEPT;
    }
    if (ops->map_fixed(ops->context, address, bytes, offset)) {
        int restored = !ops->map_anon(ops->context, address, bytes);
        if (restored) ops->copy((void *)address, (const void *)scratch, bytes);
        ops->unmap(ops->context, scratch, bytes);
        ops->release(ops->context, offset, bytes);
        return restored ? PW_PRX_DATA_KEPT : PW_PRX_DATA_BROKEN;
    }
    ops->unmap(ops->context, scratch, bytes);
    if (ops->protect && (protection & 7) != (1 | 2) && ops->protect(ops->context, address, bytes, protection & 7)) {
        /* The bytes are on direct memory and readable. A narrower protection that did not stick stays read-write
         * rather than tearing the mapping down under the module. */
    }
    return PW_PRX_DATA_DIRECT;
}
