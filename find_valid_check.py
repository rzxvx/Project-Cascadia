# Ghidra Jython script для headless-анализа
# Ищет строки, содержащие "valid" (или "Kernelcache"), находит функции,
# которые на них ссылаются, и печатает декомпилированный псевдокод.
#
# Запуск через analyzeHeadless (см. инструкцию отдельно)

from ghidra.app.decompiler import DecompInterface
from ghidra.util.task import ConsoleTaskMonitor

SEARCH_TERMS = ["valid", "Kernelcache", "kernelcache"]

program = currentProgram
listing = program.getListing()
monitor = ConsoleTaskMonitor()

decompiler = DecompInterface()
decompiler.openProgram(program)

seen_functions = set()

print("=== Ищем строки, содержащие: %s ===" % SEARCH_TERMS)

data_iter = listing.getDefinedData(True)
for data in data_iter:
    if data.hasStringValue():
        try:
            val = data.getValue()
            if val is None:
                continue
            val_str = str(val)
        except:
            continue

        for term in SEARCH_TERMS:
            if term in val_str:
                addr = data.getAddress()
                print("\n--- Найдена строка @ %s: %r ---" % (addr, val_str))

                refs = getReferencesTo(addr)
                if not refs:
                    print("  (нет ссылок на эту строку)")
                    continue

                for ref in refs:
                    from_addr = ref.getFromAddress()
                    func = getFunctionContaining(from_addr)
                    if func is None:
                        print("  Ссылка из %s — вне функции, пропускаем" % from_addr)
                        continue

                    func_key = func.getEntryPoint()
                    if func_key in seen_functions:
                        continue
                    seen_functions.add(func_key)

                    print("\n=== Функция: %s @ %s ===" % (func.getName(), func_key))

                    result = decompiler.decompileFunction(func, 60, monitor)
                    if result.decompileCompleted():
                        print(result.getDecompiledFunction().getC())
                    else:
                        print("  (декомпиляция не удалась: %s)" % result.getErrorMessage())
                break  # не проверять остальные term для этой же строки повторно

decompiler.dispose()
print("\n=== ГОТОВО ===")
