<a id="fixture-api-index-hpp"></a>
## `fixture/api_index.hpp`

Focused declarations that protect the API-reference parser's scope rules.

**Symbols:**

- [`Mode`](#fixture-api-index-hpp-mode)
  - [`Mode::direct`](#fixture-api-index-hpp-mode-direct)
  - [`Mode::queued`](#fixture-api-index-hpp-mode-queued)
- [`DocumentedMode`](#fixture-api-index-hpp-documentedmode)
  - [`DocumentedMode::immediate`](#fixture-api-index-hpp-documentedmode-immediate)
  - [`DocumentedMode::deferred`](#fixture-api-index-hpp-documentedmode-deferred)
- [`Options`](#fixture-api-index-hpp-options)
  - [`Options::label`](#fixture-api-index-hpp-options-label)
  - [`Options::count`](#fixture-api-index-hpp-options-count)
- [`Box`](#fixture-api-index-hpp-box)
  - [`Box::Box`](#fixture-api-index-hpp-box-box)
  - [`Box::get`](#fixture-api-index-hpp-box-get)
  - [`Box::value_type`](#fixture-api-index-hpp-box-value-type)
- [`Free symbols`](#fixture-api-index-hpp-free-symbols)
  - [`FIXTURE_PUBLIC`](#fixture-api-index-hpp-free-symbols-fixture-public)
  - [`FIXTURE_CHECK`](#fixture-api-index-hpp-free-symbols-fixture-check)
  - [`mode_name`](#fixture-api-index-hpp-free-symbols-mode-name)
  - [`BoxAlias`](#fixture-api-index-hpp-free-symbols-boxalias)
  - [`Sized`](#fixture-api-index-hpp-free-symbols-sized)
  - [`identity`](#fixture-api-index-hpp-free-symbols-identity)
  - [`transformer`](#fixture-api-index-hpp-free-symbols-transformer)
  - [`named::answer`](#fixture-api-index-hpp-free-symbols-named-answer)
  - [`native_windows`](#fixture-api-index-hpp-free-symbols-native-windows)
  - [`native_windows`](#fixture-api-index-hpp-free-symbols-native-windows-2)

<a id="fixture-api-index-hpp-mode"></a>
### `Mode`

A compact enum must not consume the declaration after it.

```cpp
enum class Mode;
```

<a id="fixture-api-index-hpp-mode-direct"></a>
#### `Mode::direct` — `direct = 1,`

<a id="fixture-api-index-hpp-mode-queued"></a>
#### `Mode::queued` — `queued = 2,`

<a id="fixture-api-index-hpp-documentedmode"></a>
### `DocumentedMode`

A documented enum keeps each caller-facing value's contract.

```cpp
enum class DocumentedMode : unsigned;
```

<a id="fixture-api-index-hpp-documentedmode-immediate"></a>
#### `DocumentedMode::immediate` — `immediate,`

Execute immediately.

<a id="fixture-api-index-hpp-documentedmode-deferred"></a>
#### `DocumentedMode::deferred` — `deferred,`

Wait until work is available.

<a id="fixture-api-index-hpp-options"></a>
### `Options`

Options are intentionally an aggregate.

```cpp
struct Options;
```

<a id="fixture-api-index-hpp-options-label"></a>
#### `Options::label`

```cpp
std::string label{"https://example.test"};
```
A label containing comment-looking text.

<a id="fixture-api-index-hpp-options-count"></a>
#### `Options::count`

```cpp
int count{};
```
Maximum item count.

<a id="fixture-api-index-hpp-box"></a>
### `Box`

A templated public value wrapper.

```cpp
template <typename T> class Box final;
```

<a id="fixture-api-index-hpp-box-box"></a>
#### `Box::Box`

```cpp
explicit Box(T value);
```
Construct a wrapper.

<a id="fixture-api-index-hpp-box-get"></a>
#### `Box::get`

```cpp
[[nodiscard]] const T& get() const noexcept;
```
Read the wrapped value.

<a id="fixture-api-index-hpp-box-value-type"></a>
#### `Box::value_type`

```cpp
using value_type = T;
```
The wrapped value type.

<a id="fixture-api-index-hpp-free-symbols"></a>
### `Free symbols`

<a id="fixture-api-index-hpp-free-symbols-fixture-public"></a>
#### `FIXTURE_PUBLIC`

```cpp
#define FIXTURE_PUBLIC(value) value
```

<a id="fixture-api-index-hpp-free-symbols-fixture-check"></a>
#### `FIXTURE_CHECK`

```cpp
#define FIXTURE_CHECK(value) /* implementation omitted */
```

<a id="fixture-api-index-hpp-free-symbols-mode-name"></a>
#### `mode_name`

```cpp
[[nodiscard]] inline const char* mode_name(Mode mode);
```
Return a printable mode name.

<a id="fixture-api-index-hpp-free-symbols-boxalias"></a>
#### `BoxAlias`

```cpp
template <typename T> using BoxAlias = Box<T>;
```
A template alias must remain a single symbol.

<a id="fixture-api-index-hpp-free-symbols-sized"></a>
#### `Sized`

```cpp
template <typename T> concept Sized = requires(T value) { value.size(); };
```
A concept may contain a requires-expression body.

<a id="fixture-api-index-hpp-free-symbols-identity"></a>
#### `identity`

```cpp
template <typename T> [[nodiscard]] T identity(T value);
```
A free function template.

<a id="fixture-api-index-hpp-free-symbols-transformer"></a>
#### `transformer`

```cpp
inline constexpr auto transformer{ [](int value) { /* implementation omitted */ }};
```
A public constant keeps its initializer identity without its lambda body.

<a id="fixture-api-index-hpp-free-symbols-named-answer"></a>
#### `named::answer`

```cpp
inline constexpr int answer = 42;
```
A symbol in a public nested namespace.

<a id="fixture-api-index-hpp-free-symbols-native-windows"></a>
#### `native_windows`

```cpp
inline constexpr bool native_windows = true;
```
Available when `defined(_WIN32)`.
A platform-specific declaration.

<a id="fixture-api-index-hpp-free-symbols-native-windows-2"></a>
#### `native_windows`

```cpp
inline constexpr bool native_windows = false;
```
Available when `!(defined(_WIN32))`.
A platform-specific declaration.
