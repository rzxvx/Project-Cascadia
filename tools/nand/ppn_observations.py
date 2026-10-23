"""Pure normalized PPN multi-list observations, with no hardware/firmware I/O.

Generation/ordinal are synthetic caller conventions, NOT firmware tokens.
Missing pages remain unknown. Listing a page does not establish completion,
success, durability, retirement or a recovery operation.
"""

from dataclasses import dataclass, field

if __package__:
    from .ppn_error_list import DecodeError, RequestEntry, decode_error_list
else:
    from ppn_error_list import DecodeError, RequestEntry, decode_error_list


def _synthetic_id(value, name):
    if type(value) is not int or not 0 <= value < (1 << 64):
        raise DecodeError(f'{name} must be a synthetic unsigned 64-bit integer')
    return value


@dataclass(frozen=True)
class RequestSnapshot:
    """Copied normalized request; generation is caller-provided, not on-wire."""

    generation: int
    entries: tuple

    def __post_init__(self):
        _synthetic_id(self.generation, 'generation')
        if type(self.entries) not in (list, tuple):
            raise DecodeError('entries must be a normalized list or tuple')
        copied = []
        for entry in tuple(self.entries):
            if type(entry) is not RequestEntry:
                raise DecodeError('entries must contain RequestEntry records')
            copied.append(RequestEntry(entry.page, entry.ce_index, entry.raw_status))
        records = tuple(copied)
        # Delegate duplicate-key/type rules to the existing production decoder;
        # an empty synthetic list still validates every request record.
        decode_error_list(bytes(512), records, ce_index=0, context_status=0)
        object.__setattr__(self, 'entries', records)


def _snapshot(snapshot):
    if type(snapshot) is not RequestSnapshot:
        raise DecodeError('snapshot must be a RequestSnapshot')
    # Revalidate even a frozen instance modified through object.__setattr__.
    return RequestSnapshot(snapshot.generation, snapshot.entries)


@dataclass(frozen=True)
class Observation:
    """One copied list and its explicit opaque caller context.

    ordinal distinguishes separate observations; repeating an ordinal in one
    view is ambiguous. Equal payloads with distinct ordinals are permitted.
    """

    snapshot: RequestSnapshot
    ordinal: int
    payload: bytes
    ce_index: int
    context_status: int
    decoded: object = field(init=False, repr=False)

    def __post_init__(self):
        _synthetic_id(self.ordinal, 'ordinal')
        request = _snapshot(self.snapshot)
        if type(self.payload) not in (bytes, bytearray) or len(self.payload) != 512:
            raise DecodeError('payload must contain exactly 512 bytes')
        payload = bytes(self.payload)
        decoded = decode_error_list(payload, request.entries,
                                    ce_index=self.ce_index,
                                    context_status=self.context_status)
        object.__setattr__(self, 'snapshot', request)
        object.__setattr__(self, 'payload', payload)
        object.__setattr__(self, 'decoded', decoded)


@dataclass(frozen=True)
class EntryObservation:
    """Raw caller annotation for one matching page; no flag interpretation."""

    ordinal: int
    list_index: int
    ce_index: int
    context_status: int
    raw_status: int


@dataclass(frozen=True)
class PageObservations:
    request_entry: RequestEntry
    observations: tuple

    @property
    def presence(self):
        """Only presence in captured lists: absence is always unknown."""
        return 'listed' if self.observations else 'unknown'


@dataclass(frozen=True)
class ObservationView:
    snapshot: RequestSnapshot
    observations: tuple
    pages: tuple


def reconcile_observations(expected_snapshot, observations):
    """Bind copied observations to one exact normalized request and generation.

    All input is synthetic caller data; no live generation/ownership check is
    performed. Malformed, stale, foreign or ambiguous input raises DecodeError
    without returning a partial view. Status bytes are neither ORed nor mapped
    to success/error classes. Inputs must not mutate concurrently while copied.
    """
    expected = _snapshot(expected_snapshot)
    if type(observations) not in (list, tuple):
        raise DecodeError('observations must be a list or tuple')
    copied = []
    ordinals = set()
    by_key = {(e.page, e.ce_index): [] for e in expected.entries}
    for value in tuple(observations):
        if type(value) is not Observation:
            raise DecodeError('observations must contain Observation records')
        observation = Observation(value.snapshot, value.ordinal, value.payload,
                                  value.ce_index, value.context_status)
        if observation.snapshot.generation != expected.generation:
            raise DecodeError('stale or foreign synthetic request generation')
        if observation.snapshot.entries != expected.entries:
            raise DecodeError('observation belongs to a different request snapshot')
        if observation.ordinal in ordinals:
            raise DecodeError('duplicate synthetic observation ordinal')
        ordinals.add(observation.ordinal)
        copied.append(observation)
        for index, entry in enumerate(observation.decoded.entries):
            by_key[(entry.page, entry.ce_index)].append(
                EntryObservation(observation.ordinal, index, observation.ce_index,
                                 observation.context_status, entry.raw_status))
    pages = tuple(PageObservations(entry, tuple(by_key[(entry.page, entry.ce_index)]))
                  for entry in expected.entries)
    return ObservationView(expected, tuple(copied), pages)
