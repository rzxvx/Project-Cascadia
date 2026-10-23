"""Pure synthetic PPN error-list analysis; no firmware or device interface.

Only the count/page-word layout is modeled. Request records and CE/status
context are explicitly supplied normalized fixtures, NOT a parsed C/R ABI.
127 is derived from this parser's 512-byte buffer, not a hardware limit.
"""

from dataclasses import dataclass
import struct


BUFFER_SIZE = 512
MAX_LIST_PAGES = (BUFFER_SIZE - 4) // 4


class DecodeError(ValueError):
    """Malformed or ambiguous synthetic input; nothing is partially returned."""


def _unsigned(value, bits, name):
    # bool is an int subclass; accepting it obscures malformed field input.
    if type(value) is not int or not 0 <= value < (1 << bits):
        raise DecodeError(f'{name} must be an unsigned {bits}-bit integer')
    return value


@dataclass(frozen=True)
class RequestEntry:
    """Normalized caller record; raw_status is never interpreted or changed."""

    page: int
    ce_index: int
    raw_status: int

    def __post_init__(self):
        _unsigned(self.page, 32, 'page')
        _unsigned(self.ce_index, 8, 'ce_index')
        _unsigned(self.raw_status, 8, 'raw_status')


@dataclass(frozen=True)
class DecodedErrorList:
    """Immutable matches in list order, with separate raw caller context.

    context_status is supplied by the caller, not an on-wire field. No OR,
    success/failure classification, retry advice or physical die map follows.
    """

    ce_index: int
    context_status: int
    entries: tuple


def decode_error_list(payload, request, *, ce_index, context_status):
    """Decode one exact 512-byte synthetic result against a request snapshot.

    payload: bytes or bytearray; unused padding is opaque and ignored.
    request: list/tuple of RequestEntry with unique (page, ce_index) keys.
    Each result page must match exactly one request record for ce_index.
    Invalid input raises DecodeError before returning any partial result.
    No addresses are dereferenced and no I/O occurs.
    """
    _unsigned(ce_index, 8, 'ce_index')
    _unsigned(context_status, 8, 'context_status')
    if type(payload) not in (bytes, bytearray) or len(payload) != BUFFER_SIZE:
        raise DecodeError('payload must contain exactly 512 bytes')
    data = bytes(payload)
    if len(data) != BUFFER_SIZE:
        raise DecodeError('payload changed length while copying')
    count = struct.unpack_from('<I', data)[0]
    if count > MAX_LIST_PAGES:
        raise DecodeError('count exceeds the 127-page parser buffer bound')
    if type(request) not in (list, tuple):
        raise DecodeError('request must be a normalized list or tuple')
    # Materialize the list before indexing; never keep caller-owned records.
    records = tuple(request)
    by_key = {}
    for entry in records:
        if type(entry) is not RequestEntry:
            raise DecodeError('request entries must be RequestEntry records')
        # Revalidate even records maliciously changed through object.__setattr__.
        snapshot = RequestEntry(entry.page, entry.ce_index, entry.raw_status)
        key = (snapshot.page, snapshot.ce_index)
        if key in by_key:
            raise DecodeError('duplicate request page/CE key')
        by_key[key] = snapshot
    seen = set()
    matched = []
    for index in range(count):
        page = struct.unpack_from('<I', data, 4 + index * 4)[0]
        if page in seen:
            raise DecodeError('duplicate result page')
        seen.add(page)
        entry = by_key.get((page, ce_index))
        if entry is None:
            raise DecodeError('result page has no exact request page/CE key')
        matched.append(entry)
    return DecodedErrorList(ce_index, context_status, tuple(matched))
