class Repo:
    def find(self, key, default=None, *args, strict: bool = False, **kw):
        if key in self.cache:
            return self.cache[key]
        if strict:
            raise KeyError(key)
        return default

    def name(self) -> str:
        return self._name

    def describe(self, verbose):
        if verbose:
            return f"Repo {self._name}"
        elif self.empty:
            return None
        return "Repo"

    def rows(self):
        for row in self.data:
            yield row

def build(n):
    if n < 0:
        return Repo()
    return [Repo() for _ in range(n)]

def check(a, b):
    return a == b

def log(msg):
    print(msg)
