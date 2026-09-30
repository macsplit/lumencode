package store

func Warm(keys []string) *Cache {
	cache := NewCache()
	for i, key := range keys {
		cache.Remember(key, i)
	}
	return cache
}
