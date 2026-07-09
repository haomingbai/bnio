# Buffer System

### Buffer Type Overview

```mermaid
classDiagram
    class buffer_view {
        +void* data
        +size_t size
    }

    class mutable_buffer {
        -void* data_
        -size_t size_
        +data() void*
        +size() size_t
        +view() buffer_view
    }

    class const_buffer {
        -const void* data_
        -size_t size_
        +data() const void*
        +size() size_t
    }

    class dynamic_string_buffer {
        -string* storage_
        +size() size_t
        +data() const_buffer
        +prepare(size) mutable_buffer
        +commit(size)
        +consume(size)
    }

    class dynamic_byte_vector_buffer {
        -vector~byte~* storage_
        +size() size_t
        +data() const_buffer
        +prepare(size) mutable_buffer
        +commit(size)
        +consume(size)
    }

    buffer_view <-- mutable_buffer : "view()"
```

### Factory Function `bupp::buffer()`

A set of overloaded free functions converts various types to buffer views.
These are non-owning: the caller must keep the underlying storage alive.

```cpp
auto b1 = bupp::buffer(ptr, 1024);               // void*, size_t
auto b2 = bupp::buffer(std::span(data));          // std::span<T>
auto b3 = bupp::buffer(arr);                      // std::array<T,N>&
auto b4 = bupp::buffer(vec);                      // std::vector<T>&
auto b5 = bupp::buffer(str);                      // std::string&
auto b6 = bupp::buffer(sv);                       // std::string_view
auto b7 = bupp::buffer(raw_view);                 // async_io::buffer_view
```

### Dynamic Buffer Protocol

Dynamic buffers adapt growable storage for async I/O. The pattern is:

```
prepare → use in I/O → commit → consume → repeat
```

```mermaid
sequenceDiagram
    participant User
    participant DB as dynamic_string_buffer
    participant S as std::string

    Note over S: initial: "hello" (5B)

    User->>DB: prepare(10)
    DB->>S: resize(15)
    DB-->>User: mutable_buffer{data=&S[5], size=10}

    Note over User: pass buffer to async_read

    User->>DB: commit(7)
    DB->>S: resize(12)
    Note over S: now: "hello" + 7 new bytes

    User->>DB: consume(5)
    DB->>S: erase(0, 5)
    Note over S: now: 7 bytes
```

```cpp
std::string storage;
bupp::dynamic_string_buffer dyn(storage);

// Prepare writable region
auto region = dyn.prepare(4096);

// Use region in async_read ...
// After I/O completes:

dyn.commit(actual_bytes);   // shrink to actual received size
dyn.consume(sent_bytes);    // remove processed data from front
```

Two adapters are provided:

| Adapter | Backing Store |
|---------|--------------|
| `dynamic_string_buffer` | `std::string&` |
| `dynamic_byte_vector_buffer<A>` | `std::vector<std::byte, A>&` |

Create them with `bupp::dynamic_buffer(storage)`.

---
