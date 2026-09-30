package store

type Cache struct {
	items map[string]int
}

func NewCache() *Cache {
	return &Cache{items: map[string]int{}}
}

func (c *Cache) Remember(key string, value int) {
	c.items[key] = value
}
