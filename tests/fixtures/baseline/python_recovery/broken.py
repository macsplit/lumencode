import os


def helper_broken_py():
    return os.getcwd()


def wrapper_broken_py(
    return helper_broken_py()


class WorkerBroken:
    def run(self):
        return helper_broken_py()
