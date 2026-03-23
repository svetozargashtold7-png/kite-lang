# 🪁 KITE

> Минимальный язык программирования, написанный на C

[![Version](https://img.shields.io/badge/version-1.2.0-7ee787?style=flat-square)](https://github.com)
[![Language](https://img.shields.io/badge/written_in-C-58a6ff?style=flat-square)](https://github.com)
[![Platform](https://img.shields.io/badge/platform-Linux%20%7C%20macOS%20%7C%20Termux-e3b341?style=flat-square)](https://github.com)

```
set name = "мир"
say("Привет, ${name}!")
```

---

## Что такое KITE

KITE — лёгкий интерпретируемый язык программирования общего назначения.  
Никаких точек с запятой. Никаких фигурных скобок. Только чистый читаемый код.

**Основные принципы:**
- Минимум синтаксиса — максимум возможностей
- Функции первого класса и замыкания
- ООП с наследованием и инкапсуляцией
- Стандартные библиотеки через `use`
- Работает на Android (Termux) без зависимостей

---

## Установка

### Termux (Android)
```bash
pkg install clang make
tar -xzf kite-lang.tar.gz
cd kite-lang
make CC=clang
cp kite $PREFIX/bin/kite
```

### Linux / macOS
```bash
tar -xzf kite-lang.tar.gz
cd kite-lang
make
bash install.sh
```

### Проверка
```bash
kite --version   # kite 1.2.0
kite             # запустить REPL
```

---

## Синтаксис

### Переменные
```
set x     = 42
set name  = "Алиса"
set ok    = true
set items = [1, 2, 3]
set point = {x: 10, y: 20}
```

### Условия
```
when x > 0:
    say("положительное")
orwhen x == 0:
    say("ноль")
else:
    say("отрицательное")
end
```

### Циклы
```
loop while x < 10:
    x += 1
end

loop for item in ["яблоко", "банан"]:
    say(item)
end

loop for i in range(5):
    say(i)
end
```

### Функции
```
def factorial(n):
    when n <= 1: give 1 end
    give n * factorial(n - 1)
end

say(factorial(10))   # 3628800
```

### Лямбды
```
set square = x -> x * x
set add    = (a, b) -> a + b

say(map([1,2,3,4,5], x -> x * x))   # [1, 4, 9, 16, 25]
```

### Интерполяция строк
```
set name = "KITE"
set ver  = 1.2
say("Язык ${name} версии ${ver}")
say("2 + 2 = ${2 + 2}")
```

### Обработка ошибок
```
do:
    say(1 / 0)
err ZeroDivisionError:
    say("деление на ноль!")
err:
    say("другая ошибка: ${err_msg}")
end
```

### ООП
```
obj Animal:
    set name = nil

    def init(self, name):
        self.name = name
    end

    def speak(self):
        say("${self.name} издаёт звук")
    end
end

obj Dog extends Animal:
    set _tricks = 0          # приватное поле

    def learn(self):
        self._tricks += 1
    end

    def speak(self):
        super.speak(self)
        say("${self.name}: Гав! (трюков: ${self._tricks})")
    end
end

set d = Dog.new("Шарик")
d.learn()
d.speak()
say(d is Dog)       # true
say(d is Animal)    # true
```

---

## Стандартные библиотеки

```
use math      # tan, asin, log2, clamp, lerp, E, INF...
use rand      # rand(), rand_choice(), rand_shuffle()
use string    # str_pad_left, str_count, str_rev, str_lines...
use list      # list_sum, list_zip, list_group, list_any...
use io        # file_read, file_write, file_lines, file_create...
use os        # os_env, os_shell, os_time, os_exit...
```

---

## Типы ошибок

| Тип | Когда |
|-----|-------|
| `ZeroDivisionError` | Деление на ноль |
| `NameError` | Неопределённая переменная |
| `IndexError` | Индекс за пределами списка |
| `TypeError` | Неверный тип значения |
| `IOError` | Ошибка при работе с файлами |
| `AccessError` | Доступ к приватному полю |
| `ImportError` | Неизвестная библиотека |

---

## Структура исходников

```
kite.h      — типы, AST, прототипы
lexer.c     — токенизатор
parser.c    — рекурсивный descent-парсер
value.c     — значения, reference counting, окружения
interp.c    — tree-walking интерпретатор + встроенные функции
main.c      — точка входа, REPL
Makefile
```

---

## Пример программы

```
use math
use list

obj Point:
    set x = 0
    set y = 0

    def init(self, x, y):
        self.x = x
        self.y = y
    end

    def dist(self, other):
        set dx = self.x - other.x
        set dy = self.y - other.y
        give sqrt(dx*dx + dy*dy)
    end

    def str(self):
        give "(${self.x}, ${self.y})"
    end
end

set points = [
    Point.new(0, 0),
    Point.new(3, 4),
    Point.new(6, 8)
]

set origin = points[0]
loop for p in points:
    say("${p.str()} — расстояние: ${origin.dist(p)}")
end

set distances = map(points, p -> origin.dist(p))
say("Сумма расстояний:", list_sum(distances))
```

---

## Лицензия

MIT
