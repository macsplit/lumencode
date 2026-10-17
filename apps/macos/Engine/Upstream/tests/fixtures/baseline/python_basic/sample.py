import os


def helper_py():
    return os.getcwd()


def outer_py():
    return helper_py()


class Worker:
    def run(self):
        return helper_py()
