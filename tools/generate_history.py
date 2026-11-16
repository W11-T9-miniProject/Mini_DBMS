"""Generate deterministic SQL inputs for the existing Phase 1 processor."""
import argparse
from pathlib import Path

GENRES = ("pop", "k-pop", "rock", "jazz", "hip-hop/rap", "r&b/soul", "classical",
          "alternative rock", "electronic/dance", "blues", "folk", "metal")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--count", type=int, default=1_000_000)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not 1 <= args.count <= 1_000_000:
        parser.error("count must be between 1 and 1000000")
    # Exclusive creation prevents overwriting a SQL file supplied by the user.
    with args.output.open("x", encoding="ascii", newline="\n") as out:
        for number in range(1, args.count + 1):
            genre = GENRES[(number - 1) % len(GENRES)]
            out.write("INSERT INTO play_history (event_code,user_id,song_id,genre) "
                      f"VALUES ('PLAY{number:09d}',{1 + (number-1) % 90},"
                      f"{1 + (number-1) % 10000},'{genre}');\n")


if __name__ == "__main__":
    main()
