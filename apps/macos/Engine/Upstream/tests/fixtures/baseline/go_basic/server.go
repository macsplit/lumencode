package store

import (
	"errors"
	"net/http"
)

// Store keeps items in memory.
type Store struct {
	items map[string]int
	limit int
}

type Reader interface {
	Get(key string) (int, error)
}

var ErrMissing = errors.New("missing")

func NewStore(limit int) *Store {
	return &Store{items: map[string]int{}, limit: limit}
}

func (s *Store) Get(key string) (int, error) {
	value, ok := s.items[key]
	if !ok {
		return 0, ErrMissing
	}
	return value, nil
}

func (s *Store) Put(key string, value int) error {
	if len(s.items) >= s.limit {
		return errors.New("full")
	}
	s.items[key] = value
	return nil
}

func Register(mux *http.ServeMux, s *Store) {
	mux.HandleFunc("GET /items/{key}", func(w http.ResponseWriter, r *http.Request) {
		_, _ = s.Get(r.PathValue("key"))
	})
	mux.HandleFunc("/health", health)
}

func health(w http.ResponseWriter, r *http.Request) {}
