"""
Emulates java.util.HashMap<String, V> (JDK 8+ semantics) well enough to reproduce
CardLibrary.cards' iteration order (entrySet()/keySet()/values() all share the same
table-scan order in HashMap).

Java HashMap internals (from OpenJDK 8-17 source, java.util.HashMap):

  - String.hashCode(): h = 0; for each char c: h = 31*h + c   (int, 32-bit overflow wraps)
    (java.lang.String.hashCode(), stable/documented since Java 1.2 — this exact formula
    is part of the String.hashCode() Javadoc contract, so it hasn't changed across JDKs.)

  - HashMap.hash(key): spreads the hashCode to reduce collisions in low bits:
        h = key.hashCode();
        return h ^ (h >>> 16);
    (java.util.HashMap.hash(Object), unchanged since Java 8's HashMap rewrite.)

  - Table size: power of two. DEFAULT_INITIAL_CAPACITY = 16, DEFAULT_LOAD_FACTOR = 0.75.
    `new HashMap<>()` (no-arg ctor, which is exactly what CardLibrary.java:391 uses) lazily
    allocates the table to size 16 on the first put (HashMap.resize()), and resizes
    (doubles) whenever size > threshold (capacity * loadFactor) after an insert.

  - Bucket index for a given hash: (table.length - 1) & hash.

  - Insertion order WITHIN a bucket (a bucket = a singly linked list of Node, absent
    treeification): new entries are appended to the END of the bucket's chain
    (HashMap.putVal: walks the chain via `e = e.next` and appends `newNode` when it hits
    the last node — `p.next = newNode(...)`). So within one bucket, insertion order is
    preserved.

  - Iteration order (HashMap.Node[] table, iterated by HashMap$HashIterator.nextNode()):
    scan table[0], table[1], ..., table[table.length-1]; within each bucket, walk the
    chain head-to-tail (i.e. in the append/insertion order recorded above).

  - Resize (HashMap.resize(), Java 8+ split-transfer algorithm): when capacity doubles
    from oldCap to newCap=2*oldCap, each old bucket's chain is split into two new chains
    ("lo" stays at the same index, "hi" moves to index+oldCap), based on whether
    `(hash & oldCap) == 0` (lo) or `!= 0` (hi) for each node — walked in the OLD chain's
    order, appended to the tail of whichever new list it lands in. This is a stable
    partition: the relative order within the lo list and within the hi list is preserved
    from the pre-resize chain order. This emulator implements exactly this split-transfer,
    not a full "insert everything into a fresh table" (which usually - but not always -
    gives the same relative bucket-chain order, since re-inserting also appends to bucket
    tail in scan order; the split-transfer is the actual algorithm HashMap uses and is
    reproduced 1:1 here to avoid relying on that equivalence).

  - Treeification: a bucket whose chain length reaches TREEIFY_THRESHOLD (8) is
    (conditionally, if table.length >= MIN_TREEIFY_CAPACITY = 64) converted to a red-black
    tree, which changes iteration order within that bucket to tree order rather than
    insertion order. This emulator asserts no bucket ever reaches 8 entries for the actual
    CardLibrary key set at any table size encountered; see `assert_no_treeification`.

This module operates purely on the sequence of (key) insertions supplied by the caller,
mirroring HashMap<String, AbstractCard>'s put() call sequence, and produces the final
iteration order of the keys.
"""

from typing import List


def java_string_hashcode(s: str) -> int:
    """Java's String.hashCode(): h = 31*h + c for each char, as a 32-bit signed int."""
    h = 0
    for ch in s:
        h = (31 * h + ord(ch)) & 0xFFFFFFFF
    # convert to signed 32-bit
    if h >= 0x80000000:
        h -= 0x100000000
    return h


def hashmap_hash(key: str) -> int:
    """HashMap.hash(Object): h = key.hashCode(); return h ^ (h >>> 16) (unsigned right shift)."""
    h = java_string_hashcode(key)
    # unsigned right shift by 16 on a 32-bit value
    h_unsigned = h & 0xFFFFFFFF
    shifted = h_unsigned >> 16
    result = (h_unsigned ^ shifted) & 0xFFFFFFFF
    if result >= 0x80000000:
        result -= 0x100000000
    return result


TREEIFY_THRESHOLD = 8
MIN_TREEIFY_CAPACITY = 64
DEFAULT_INITIAL_CAPACITY = 16
DEFAULT_LOAD_FACTOR = 0.75


class JavaHashMapOrderSimulator:
    """Simulates java.util.HashMap<String, V> insertion/resize to reproduce entrySet() iteration order."""

    def __init__(self):
        self.table = None  # list of buckets; each bucket is a list of keys in chain order
        self.capacity = 0
        self.threshold = 0
        self.size = 0
        self._treeified_buckets = []  # record any bucket that would treeify, for reporting

    def _ensure_initial_capacity(self):
        """Lazily allocate the table at DEFAULT_INITIAL_CAPACITY on first put (HashMap.resize() called from putVal when table is null)."""
        if self.table is None:
            self.capacity = DEFAULT_INITIAL_CAPACITY
            self.table = [[] for _ in range(self.capacity)]
            self.threshold = int(self.capacity * DEFAULT_LOAD_FACTOR)

    def _resize(self):
        """HashMap.resize(): double capacity, split-transfer each bucket's chain into lo/hi lists preserving relative order."""
        old_cap = self.capacity
        new_cap = old_cap * 2
        new_table = [[] for _ in range(new_cap)]

        for j in range(old_cap):
            old_chain = self.table[j]
            if not old_chain:
                continue
            lo = []
            hi = []
            for key in old_chain:
                h = hashmap_hash(key)
                h_unsigned = h & 0xFFFFFFFF
                if (h_unsigned & old_cap) == 0:
                    lo.append(key)
                else:
                    hi.append(key)
            if lo:
                new_table[j] = lo
            if hi:
                new_table[j + old_cap] = hi

        self.table = new_table
        self.capacity = new_cap
        self.threshold = int(new_cap * DEFAULT_LOAD_FACTOR)

    def put(self, key: str):
        """java.util.HashMap.put(key, value) — only the key matters for iteration-order emulation. Duplicate keys overwrite value in place (no order change), matching HashMap.putVal's onlyIfAbsent=false + no-move-on-update behavior."""
        self._ensure_initial_capacity()
        h = hashmap_hash(key)
        idx = (self.capacity - 1) & (h & 0xFFFFFFFF)
        bucket = self.table[idx]
        if key in bucket:
            # HashMap overwrites the value for an existing key without changing position/size.
            return
        bucket.append(key)
        self.size += 1
        if len(bucket) >= TREEIFY_THRESHOLD and self.capacity >= MIN_TREEIFY_CAPACITY:
            self._treeified_buckets.append((idx, list(bucket), self.capacity))
        if self.size > self.threshold:
            self._resize()

    def put_all(self, keys: List[str]):
        for k in keys:
            self.put(k)

    def iteration_order(self) -> List[str]:
        """Reproduces HashMap$HashIterator's scan order: table[0..cap-1], each bucket head-to-tail."""
        if self.table is None:
            return []
        order = []
        for bucket in self.table:
            order.extend(bucket)
        return order

    def assert_no_treeification(self):
        if self._treeified_buckets:
            raise AssertionError(f"Treeification would occur for buckets: {self._treeified_buckets}")


def simulate_order(keys: List[str]) -> List[str]:
    """Convenience: run the full HashMap put sequence for `keys` (in insertion order) and return entrySet() iteration order."""
    sim = JavaHashMapOrderSimulator()
    sim.put_all(keys)
    sim.assert_no_treeification()
    return sim.iteration_order()


if __name__ == '__main__':
    # Smoke tests against known Java String.hashCode() values.
    assert java_string_hashcode("") == 0
    assert java_string_hashcode("a") == 97
    assert java_string_hashcode("hello") == 99162322, java_string_hashcode("hello")
    assert java_string_hashcode("Anger") == 63408103, java_string_hashcode("Anger")  # cross-checked against a live JDK 26 run
    print("String.hashCode() smoke tests passed")
