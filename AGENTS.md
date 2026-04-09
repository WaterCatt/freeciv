# AGENTS.md

## Проект

Freeciv (fork) с целью реализации:

👉 Replay System (система записи и воспроизведения игр)

---

## Цели проекта

Реализовать систему:

1. записи игровых событий на сервере
2. сохранения replay-файлов (.fcreplay)
3. детерминированного воспроизведения
4. UI управления воспроизведением (Qt)
5. поддержки перемотки и смены скорости

---

## Среда разработки

- OS: Ubuntu
- IDE: CLion
- Agent: OpenCode
- Compiler: GCC / G++
- Build system: Meson + Ninja
- GUI: Qt6
- Build dir: `buildDir`

---

## Сборка проекта

### Configure
```bash
meson setup buildDir -Daudio=none -Dclients=qt -Dfcmp=qt -Dnls=false