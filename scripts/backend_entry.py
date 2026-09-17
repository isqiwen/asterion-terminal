"""Frozen executable entry; multiprocessing must divert before CLI parsing."""

import multiprocessing

if __name__ == "__main__":
    multiprocessing.freeze_support()
    from asterion.runtime.cli import main

    main()
