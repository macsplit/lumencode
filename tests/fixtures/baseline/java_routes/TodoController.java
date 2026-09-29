package app.web;

import org.springframework.web.bind.annotation.*;
import java.util.List;

@RestController
@RequestMapping("/api/todos")
public class TodoController {

    @GetMapping
    public List<String> list() {
        return List.of();
    }

    @GetMapping("/{id}")
    public String get(@PathVariable String id) {
        return id;
    }

    @PostMapping(value = "/")
    public String create(@RequestBody String body) {
        return body;
    }

    @RequestMapping(value = "/{id}", method = RequestMethod.DELETE)
    public void delete(@PathVariable String id) {
    }
}
