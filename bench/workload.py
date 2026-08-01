"""Shared workload generator — identical parameters for both engines (fixed seed)."""
import random

N_ROWS = 10_000
N_POINT = 1_000
N_RANGE = 100
RANGE_WIDTH = 1_000
N_FULL = 10
SEED = 42


def gen_values():
    """value column for rows id=1..N_ROWS (insertion order)."""
    rng = random.Random(SEED)
    return [rng.randrange(0, 1_000_000_000) for _ in range(N_ROWS)]


def gen_point_ids():
    rng = random.Random(SEED + 1)
    return [rng.randrange(1, N_ROWS + 1) for _ in range(N_POINT)]


def gen_range_starts():
    """start a, range covers [a, a+RANGE_WIDTH-1], kept inside table."""
    rng = random.Random(SEED + 2)
    return [rng.randrange(1, N_ROWS - RANGE_WIDTH + 2) for _ in range(N_RANGE)]
