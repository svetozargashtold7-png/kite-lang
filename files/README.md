# KITE — Minimal Programming Language

A clean, minimal, dynamically-typed programming language written in C.
No dependencies beyond libc and libm.

## Build

```
make
```

Requires GCC (or any C11 compiler). Tested on Linux.

## Run

```bash
./kite program.kite       # run a file
./kite                    # interactive REPL
./kite --help
./kite --version
```

## Language Reference

### Comments
```
# this is a comment
```

### Variables
```
set x = 42
set name = "Alice"
set ok = true
set nothing = nil
```

### Reassignment & compound ops
```
x = x + 1
x += 10
x -= 1
x *= 2
x /= 4
```

### Types
| Type    | Example            |
|---------|--------------------|
| num     | `42`, `3.14`       |
| str     | `"hello"`          |
| bool    | `true`, `false`    |
| nil     | `nil`              |
| list    | `[1, 2, 3]`        |
| map     | `{key: value}`     |
| fn      | `def f(x): ... end`|

### Arithmetic & Operators
```
+  -  *  /  %  ^        # math (^ is power)
..                       # string concat
==  !=  <  >  <=  >=   # comparison
and  or  not             # logic
```

### Strings
```
set s = "hello" .. " world"   # concat with ..
set s = "ha" * 3              # repeat → "hahaha"
set c = s[0]                  # char at index
```

### Functions
```
def add(a, b):
  give a + b
end

say(add(3, 4))    # → 7
```
`give` is the return keyword.

### Lambdas
```
set square = x -> x * x          # single param, expression body
set add    = (a, b) -> a + b     # multi param

# block body lambda
set greet = name ->:
  say("Hello, " .. name)
  give true
end
```

### Conditionals
```
when x > 0:
  say("positive")
orwhen x == 0:
  say("zero")
else:
  say("negative")
end
```

### Loops
```
# while loop
loop while i < 10:
  i += 1
end

# for-in loop
loop for item in [1, 2, 3]:
  say(item)
end

# range
loop for i in range(5):       # 0..4
  say(i)
end
loop for i in range(1, 6):    # 1..5
  say(i)
end
loop for i in range(0, 10, 2): # 0,2,4,6,8
  say(i)
end

# flow control
break    # exit loop
next     # skip to next iteration
```

### Lists
```
set a = [10, 20, 30]
a[0]                    # → 10
a[-1]                   # → 30  (negative index)
push(a, 40)             # append
pop(a)                  # remove & return last
len(a)                  # length
slice(a, 1, 3)          # sublist
reverse(a)              # reversed copy
concat(a, b)            # combine two lists
sort(a)                 # sorted copy
sort(a, (x,y) -> ...)   # sorted with comparator
has(a, 20)              # membership test
```

### Maps
```
set m = {name: "Bob", age: 25}
m["name"]               # → "Bob"
m.age                   # → 25 (dot access)
m["city"] = "Paris"     # set key
keys(m)                 # list of keys
vals(m)                 # list of values
has(m, "name")          # key existence test
```

### Closures
```
def make_adder(n):
  give x -> x + n
end

set add5 = make_adder(5)
say(add5(10))    # → 15
```

### Higher-Order Functions
```
set nums = range(1, 11)

map(nums, x -> x * x)            # [1, 4, 9, ...]
filter(nums, x -> x % 2 == 0)    # [2, 4, 6, ...]
reduce(nums, (a,b) -> a+b, 0)    # 55
```

### Objects (maps as structs)
```
def make_point(x, y):
  give {x: x, y: y}
end

def dist(p, q):
  set dx = q["x"] - p["x"]
  set dy = q["y"] - p["y"]
  give sqrt(dx*dx + dy*dy)
end

set p = make_point(3, 4)
say(dist(make_point(0,0), p))    # → 5
```

## Built-in Functions

### I/O
| Function       | Description                      |
|----------------|----------------------------------|
| `say(...)`     | Print values separated by spaces |
| `input(prompt)`| Read line from stdin             |

### Type
| Function   | Description                       |
|------------|-----------------------------------|
| `type(v)`  | Returns type name as string       |
| `str(v)`   | Convert to string                 |
| `num(v)`   | Convert to number                 |

### Lists
| Function            | Description                    |
|---------------------|--------------------------------|
| `len(v)`            | Length of list/str/map         |
| `push(list, val)`   | Append to list (in-place)      |
| `pop(list)`         | Remove & return last element   |
| `slice(v, s, e)`    | Sub-list or sub-string         |
| `reverse(v)`        | Reversed copy                  |
| `concat(a, b)`      | Concatenate two lists          |
| `sort(list)`        | Sorted copy (natural order)    |
| `sort(list, fn)`    | Sorted copy with comparator    |
| `has(v, x)`         | Test membership / key presence |
| `map(list, fn)`     | Transform each element         |
| `filter(list, fn)`  | Keep matching elements         |
| `reduce(list,fn,i)` | Fold to single value           |

### Maps
| Function      | Description          |
|---------------|----------------------|
| `keys(map)`   | List of keys         |
| `vals(map)`   | List of values       |

### Strings
| Function                 | Description              |
|--------------------------|--------------------------|
| `len(s)`                 | String length            |
| `split(s, sep)`          | Split into list          |
| `join(list, sep)`        | Join list into string    |
| `slice(s, start, end)`   | Substring                |
| `upcase(s)`              | UPPERCASE                |
| `downcase(s)`            | lowercase                |
| `trim(s)`                | Strip whitespace         |
| `replace(s, from, to)`   | Replace substring        |
| `index_of(s, sub)`       | Find position (-1=none)  |
| `has(s, sub)`            | Substring test           |

### Math
| Function       | Description           |
|----------------|-----------------------|
| `sqrt(x)`      | Square root           |
| `abs(x)`       | Absolute value        |
| `floor(x)`     | Round down            |
| `ceil(x)`      | Round up              |
| `round(x)`     | Round to nearest      |
| `min(a, b, …)` | Minimum               |
| `max(a, b, …)` | Maximum               |
| `sin(x)`       | Sine                  |
| `cos(x)`       | Cosine                |
| `log(x)`       | Natural logarithm     |
| `range(n)`     | List 0..n-1           |
| `range(a,b)`   | List a..b-1           |
| `range(a,b,s)` | List a..b-1 step s    |
| `PI`           | 3.14159…              |
| `TAU`          | 6.28318…              |

## REPL Commands
```
.help    show quick reference
.env     show all defined variables
.quit    exit
```

## Source Structure

```
kite.h       types, structs, prototypes (shared header)
lexer.c      tokenizer
parser.c     recursive descent parser → AST
value.c      value types + reference counting + environment
interp.c     tree-walking interpreter + all builtins
main.c       entry point: file runner + REPL
Makefile
```

## Example Programs

### FizzBuzz
```
loop for i in range(1, 31):
  when i % 15 == 0: say("FizzBuzz")
  orwhen i % 3 == 0: say("Fizz")
  orwhen i % 5 == 0: say("Buzz")
  else: say(i)
  end
end
```

### Fibonacci
```
def fib(n):
  when n <= 1: give n end
  give fib(n-1) + fib(n-2)
end

loop for i in range(10):
  say(fib(i))
end
```

### Quicksort
```
def qsort(arr):
  when len(arr) <= 1: give arr end
  set pivot = arr[floor(len(arr) / 2)]
  set left  = filter(arr, x -> x < pivot)
  set mid   = filter(arr, x -> x == pivot)
  set right = filter(arr, x -> x > pivot)
  give concat(concat(qsort(left), mid), qsort(right))
end

say(qsort([3, 6, 8, 10, 1, 2, 1]))
```
