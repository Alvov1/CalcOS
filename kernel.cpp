// Эта инструкция обязательно должна быть первой, т.к. этот код компилируется в бинарный,
// и загрузчик передает управление по адресу первой инструкции бинарного образа ядра ОС.
__asm("jmp kmain");

#include <limits.h>

#define VIDEO_BUF_PTR (0xb8000)
#define IDT_TYPE_INTR (0x0E)
#define IDT_TYPE_TRAP (0x0F)
// Селектор секции кода, установленный загрузчиком ОС
#define GDT_CS (0x8)
#define PIC1_PORT (0x20)
#define CURSOR_PORT (0x3D4)
#define VIDEO_WIDTH (80) // Ширина текстового экрана

#define STRING_LEN 80
#define COLUMN_LEN 25
/* Размер одного пикселя.*/
#define PIXEL 2
/* Адрес, где записан выбранный пользователем номер цвета. */
#define ARG 0x9000

/* Скан коды клавиш. */
#define ENTER 28
#define BACKSPACE 14
#define TAB 15
#define RSHIFT 42
#define LSHIFT 54
#define CAPSLOCK 58
#define SPACE 0x39

#define MINUS '-'
#define PLUS '+'
#define MULTIPLY '*'
#define DIVIDE '/'

/* Цветовое оформление. */
unsigned mainColor = 2;

/* Номер строки в памяти видеоадаптера. */
unsigned stringCount = 0;
/* Номер столбца в памяти видеоадаптера. */
unsigned columnCount = 0;

/* Зажат lshift или rshift. */
bool shift = false;
/* CapsLock. */
bool capsLock = false;
/* ------------------------ Обработка строк -------------------------- */
int strlen(const char* str){
    int i = 0;
    for(; *str != 0; i++, str++);
    return i;
}
int strncmp(const char* first, const char* second, const unsigned size){
    for(int i = 0; i < size; i++)
        if(first[i] != second[i])
            return -1;
    return 0;
}
/* -------------------- Чтение и запись в порт ----------------------- */
static inline unsigned char inb (unsigned short port){ // Чтение из порта
    unsigned char data;
    asm volatile ("inb %w1, %b0" : "=a" (data) : "Nd" (port));
    return data;
}
static inline void outb (unsigned short port, unsigned char data){ // Запись
    asm volatile ("outb %b0, %w1" : : "a" (data), "Nd" (port));
}
static inline void outw (unsigned int port, unsigned int data){
    asm volatile ("outw %w0, %w1" : : "a" (data), "Nd" (port));
}
/* ----------------------- Перевод курсора --------------------------- */
/* Переводит курсор на строку strnum (0 – самая верхняя) в позицию
 * pos на этой строке (0 – самое левое положение).*/
void cursor_moveto(unsigned int strnum, unsigned int pos){
    unsigned short new_pos = (strnum * VIDEO_WIDTH) + pos;
    outb(CURSOR_PORT, 0x0F);
    outb(CURSOR_PORT + 1, (unsigned char)(new_pos & 0xFF));
    outb(CURSOR_PORT, 0x0E);
    outb(CURSOR_PORT + 1, (unsigned char)( (new_pos >> 8) & 0xFF));
}
/* -------------------- Обработка видеовывода ------------------------ */
void clean(){
    unsigned char* window = (unsigned char*) VIDEO_BUF_PTR;
    /* Заменяем символы в видеобуфере. Каждый пиксель занимает 2 байта:
     * первый - аски код, второй - символ. Заменяем только первые. */
    for(unsigned i = 0; i < STRING_LEN * COLUMN_LEN * PIXEL; i += 2)
        window[i] = 0;

    cursor_moveto(0, 0);
    stringCount = 0;
    columnCount = 0;
}

void endLine(){
    if(stringCount >= 25)
        clean();
    else {
        stringCount++;
        columnCount = 0;
        cursor_moveto(stringCount, 0);
    }
}

/* Замена значений пикселя на экране вывода. */
void pixelSet(unsigned char *place, const unsigned char c){
    *place = c;
    *(place + 1) = mainColor;
}

/* Вывод строки на экран. */
int outStr(const char* ptr){
    /* Очищаем память видеовывода, если он переполнен. */
    if(stringCount > 24) clean();
    unsigned char* video_buf = (unsigned char*) VIDEO_BUF_PTR;
    video_buf += STRING_LEN * PIXEL * stringCount;
    for(; *ptr; ptr++, video_buf += 2)
        pixelSet(video_buf, *ptr);
    stringCount++;
    cursor_moveto(stringCount, 0);
    return 0;
}

/* Вывод символа на экран. */
int outChar(const char c){
    /* Окно вывода заполнено. */
    if(stringCount == COLUMN_LEN - 1 && columnCount == STRING_LEN - 1) {
        clean();
        stringCount = 0;
        columnCount = 0;
    }
    unsigned char* video_buf = (unsigned char*) VIDEO_BUF_PTR;

    video_buf += STRING_LEN * PIXEL * stringCount;
    video_buf += PIXEL * columnCount;

    pixelSet(video_buf, c);
    if(columnCount == 80){
        stringCount++;
        columnCount = 0;
    } else columnCount++;
    cursor_moveto(stringCount, columnCount);
    return 0;
}

/* Клавиша BackSpace.*/
void delChar(){
    if(stringCount > 0 || columnCount > 1) {
        if(columnCount == 0){
            stringCount--;
            columnCount = 80;
        }
        columnCount--;
        unsigned char *video_buf = (unsigned char *) VIDEO_BUF_PTR;

        video_buf += STRING_LEN * PIXEL * stringCount;
        video_buf += PIXEL * columnCount;

        pixelSet(video_buf, 0);
        cursor_moveto(stringCount, columnCount);
    }
}
/* -------------------------- Прерывания ----------------------------- */
// Структура описывает данные об обработчике прерывания
struct idt_entry{
    unsigned short base_lo; // Младшие биты адреса обработчика
    unsigned short segm_sel; // Селектор сегмента кода
    unsigned char always0; // Этот байт всегда 0
    unsigned char flags; // Флаги тип. Флаги: P, DPL, Типы - это константы - IDT_TYPE...
    unsigned short base_hi; // Старшие биты адреса обработчика
} __attribute__((packed)); // Выравнивание запрещено
// Структура, адрес которой передается как аргумент команды lidt
struct idt_ptr{
    unsigned short limit;
    unsigned int base;
} __attribute__((packed)); // Выравнивание запрещено
struct idt_entry g_idt[256]; // Реальная таблица IDT
struct idt_ptr g_idtp; // Описатель таблицы для команды lidt
// Пустой обработчик прерываний. Другие обработчики могут быть реализованы по этому шаблону
void default_intr_handler(){
    asm("pusha");
// ... (реализация обработки)
    asm("popa; leave; iret");
}
typedef void (*intr_handler)();
void intr_reg_handler(int num, unsigned short segm_sel, unsigned short flags, intr_handler hndlr){
    unsigned int hndlr_addr = (unsigned int) hndlr;
    g_idt[num].base_lo = (unsigned short) (hndlr_addr & 0xFFFF);
    g_idt[num].segm_sel = segm_sel;
    g_idt[num].always0 = 0;
    g_idt[num].flags = flags;
    g_idt[num].base_hi = (unsigned short) (hndlr_addr >> 16);
}
// Функция инициализации системы прерываний: заполнение массива с адресами обработчиков
void intr_init(){
    int i;
    int idt_count = sizeof(g_idt) / sizeof(g_idt[0]);
    for(i = 0; i < idt_count; i++)
        intr_reg_handler(i, GDT_CS, 0x80 | IDT_TYPE_INTR,
                         default_intr_handler); // segm_sel=0x8, P=1, DPL=0, Type=Intr
}
void intr_start(){
    int idt_count = sizeof(g_idt) / sizeof(g_idt[0]);
    g_idtp.base = (unsigned int) (&g_idt[0]);
    g_idtp.limit = (sizeof (struct idt_entry) * idt_count) - 1;
    asm("lidt %0" : : "m" (g_idtp) );
}
void intr_enable(){
    asm("sti");
}
void intr_disable(){
    asm("cli");
}
/* ----------------------- Введенный символ -------------------------- */
int Expression(const char* str){
    char expression[75] = {0};
    const char* test = expression;

    unsigned len = 0;
    /* ----------------- Удаляем из строки все символы кроме цифр и арифметических операций. ---------------------- */
    for(int i = 0; i < strlen(str); i++){
        if(str[i] == ' ' || str[i] == '\t' || str[i] == '\n')
            continue;
        if(str[i] >= '0' && str[i] <= '9') {
            expression[len] = str[i];
            len++;
        } else {
            if(str[i] == MINUS) {
                if(str[i + 1] == MINUS)
                    continue;
                if(str[i + 1] == PLUS || str[i + 1] == DIVIDE || str[i + 1] == MULTIPLY)
                    return outStr("Error: expression is incorrect");
                expression[len] = MINUS;
                len++;
            }

            if(str[i] == PLUS) {
                if (str[i + 1] == PLUS || str[i + 1] == MINUS)
                    continue;
                if (str[i + 1] == MULTIPLY || str[i + 1] == DIVIDE)
                    return outStr("Error: expression is incorrect");
                expression[len] = PLUS;
                len++;
            }

            if(str[i] == MULTIPLY) {
                if(str[i + 1] == MULTIPLY || str[i + 1] == DIVIDE)
                    return outStr("Error: expression is incorrect");
                expression[len] = MULTIPLY;
                len++;
            };
            if(str[i] == DIVIDE) {
                if(str[i + 1] == MULTIPLY || str[i + 1] == DIVIDE)
                    return outStr("Error: expression is incorrect");
                expression[len] = DIVIDE;
                len++;
            }
            if(str[i] >= 'a' && str[i] <= 'z' || str[i] >= 'A' && str[i] <= 'Z') {
                return outStr("Error: expression is incorrect");
            }
        }
    }
    if(len == 0) return outStr("Error: expression is incorrect");


    int operation = -1;

    /* -------------------------- Находим первую операцию умножения или деления ----------------------------------- */
    for(int i = 0; i < len && operation == -1; i++)
        if(expression[i] == MULTIPLY || expression[i] == DIVIDE)
            operation = i;


    /* ----------------- Вычисляем все умножения и деления. Заменяем в строке на результат ------------------------ */
    typedef struct Number{
        char Number[75];
        int len;
    }Number;

    Number left;
    Number right;
    Number result;
    Number resultStr;
    left.len = 0;
    right.len = 0;
    result.len = 0;
    resultStr.len = 0;

    for(int i = 0; i < 75; i++){
        right.Number[i] = 0;
        left.Number[i] = 0;
        result.Number[i] = 0;
        resultStr.Number[i] = 0;
    }

    /* Если правое число в умножении или делении отрицательное. */
    int rightNumberFlag = 0;
    char rightNumberOperation = 0;

    while(operation != -1){
        if(operation == 0 || operation == len - 1) return -1;

        int leftSpace = -1;
        int rightSpace = -1;

        for(int i = operation - 1; i >= 0 && leftSpace == -1; i--)
            if(expression[i] == PLUS || expression[i] == MINUS ||
               expression[i] == DIVIDE || expression[i] == MULTIPLY) leftSpace = i;

        for(int i = operation + 2; i < len && rightSpace == -1; i++)
            if(expression[i] == PLUS || expression[i] == MINUS ||
               expression[i] == DIVIDE || expression[i] == MULTIPLY) rightSpace = i;

        /* У правого числа есть унарный плюс или минус.*/
        if(expression[operation + 1] == MINUS || expression[operation + 1] == PLUS) {
            rightNumberFlag = 1;
            rightNumberOperation = expression[operation + 1];
        }

        if(rightSpace == -1) rightSpace = len;
        for(int i = leftSpace + 1; i < operation; i++, left.len++)
            left.Number[left.len] = expression[i];

        if(expression[operation + 1] != PLUS && expression[operation + 1] != MINUS)
            for(int i = operation + 1; i < rightSpace; i++, right.len++)
                right.Number[right.len] = expression[i];
        else
            for(int i = operation + 2; i < rightSpace; i++, right.len++)
                right.Number[right.len] = expression[i];

        unsigned long rightNumber = 0;
        unsigned long leftNumber = 0;

        for(int i = 0; i < right.len; i++) {
            if(rightNumber > INT_MAX)
                return outStr("Error: integer overflow");
            rightNumber *= 10;
            rightNumber += (right.Number[i] - '0');
            right.Number[i] = 0;
        }

        for(int i = 0; i < left.len; i++) {
            if(leftNumber > INT_MAX)
                return outStr("Error: integer overflow");
            leftNumber *= 10;
            leftNumber += (left.Number[i] - '0');
            left.Number[i] = 0;
        }

        unsigned long res = 0;

        switch(expression[operation]) {
            case MULTIPLY:
                res = leftNumber * rightNumber;
                if(res < leftNumber || res < rightNumber)
                    return outStr("Error: integer overflow");
                break;
            case DIVIDE:
                if(rightNumber != 0)
                    res = leftNumber / rightNumber;
                else
                    return outStr("Error: division by 0");
                break;
            default:
                return outStr("Error");
        }

        if(res > INT_MAX)
            return outStr("Error: integer overflow");
        if(res != 0)
            for(int i = 0; res != 0; i++, result.len++) {
                result.Number[i] =  '0' + res % 10;
                res /= 10;
            }
        else {
            result.Number[0] = '0';
            result.len = 1;
        }

        int swift = right.len + left.len - result.len + 1;

        switch(rightNumberFlag){
            case 1:
                switch(expression[leftSpace]){
                    case MINUS:
                        rightNumberFlag = 0;
                        expression[leftSpace] = PLUS;
                        for(int i = 0; i < result.len; i++) {
                            expression[leftSpace + result.len - i] = result.Number[i];
                            result.Number[i] = 0;
                        }
                        swift++;
                        for(int i = leftSpace + result.len + 1; i < len; i++)
                            expression[i] = expression[i + swift];
                        break;
                    case PLUS:
                        rightNumberFlag = 0;
                        expression[leftSpace] = PLUS;
                        for(int i = 0; i < result.len; i++) {
                            expression[leftSpace + result.len - i] = result.Number[i];
                            result.Number[i] = 0;
                        }
                        swift++;
                        for(int i = leftSpace + result.len + 1; i < len; i++)
                            expression[i] = expression[i + swift];
                        break;
                    default:
                        rightNumberFlag = 0;
                        expression[leftSpace + 1] = rightNumberOperation;
                        for(int i = 0; i < result.len; i++) {
                            expression[leftSpace + result.len - i + 1] = result.Number[i];
                            result.Number[i] = 0;
                        }

                        for(int i = leftSpace + result.len + 2; i < len; i++)
                            expression[i] = expression[i + swift];
                        break;
                }
                break;
            case 0:
                for(int i = 0; i < result.len; i++) {
                    expression[leftSpace + result.len - i] = result.Number[i];
                    result.Number[i] = 0;
                }
                for(int i = leftSpace + result.len + 1; i < len; i++)
                    expression[i] = expression[i + swift];
                break;
        }

        len -= (right.len + left.len - result.len + 1);
        operation = -1;
        for(int i = 0; i < len && operation == -1; i++)
            if(expression[i] == MULTIPLY || expression[i] == DIVIDE)
                operation = i;

        result.len = 0;
        left.len = 0;
        right.len = 0;
    }

    long mainResult = 0;
    long resultBefore = 0;

    unsigned adding = 0;
    unsigned start = 0;
    unsigned finish = 0;

    for(int i = 0; i < len; i++){
        while(expression[i] == '+' || expression[i] == '-')
            i++;
        start = i;
        for(finish = start; expression[finish] >= '0' && expression[finish] <= '9'; finish++) {
            adding *= 10;
            adding += expression[finish] - '0';
        }
        if(start == 0 || expression[start - 1] == '+') {
            resultBefore = mainResult;
            mainResult += adding;
            if(resultBefore > mainResult)
                return outStr("Error: integer overflow");
        }
        if(expression[start - 1] == '-') {
            resultBefore = mainResult;
            mainResult -= adding;
            if(resultBefore < mainResult)
                return outStr("Error: integer overflow");
        }
        i = finish;
        adding = 0;
    }

    resultStr.len = 0;
    if(mainResult < 0){
        outChar('-');
        mainResult *= -1;
    }
    for(resultStr.len; mainResult > 0; resultStr.len++){
        resultStr.Number[resultStr.len] = mainResult % 10 + '0';
        mainResult /= 10;
    }

    if(resultStr.len != 0) {
        for (int i = resultStr.len - 1; i >= 0; i--) {
            outChar(resultStr.Number[i]);
            resultStr.Number[i] = 0;
        }
        resultStr.len = 0;
    } else
        outChar('0');
    endLine();

    return 0;
}

void command(){
    char str[80] = {0};
    unsigned char* stringBeg = (unsigned char*) VIDEO_BUF_PTR;
    stringBeg += STRING_LEN * PIXEL * stringCount;

    unsigned char* temp = stringBeg;
    /* Пропускаем пробелы перед командой. */
    while(*temp == ' ')
        temp += 2;

    for(int i = 0; i < columnCount; i++, temp += 2)
        str[i] = static_cast<char>(*temp);

    stringCount++;
    columnCount = 0;
    cursor_moveto(stringCount, 0);

    if(strncmp(str, "info", 4) == 0){
        outStr("    Alexander Lvov. 4851003/90001");
        outStr("    CalcOS. FASM, GCC");
        switch(mainColor) {
            case 2:
                outStr("    Chosen color - Green");
                break;
            case 1:
                outStr("    Chosen color - Blue");
                break;
            case 4:
                outStr("    Chosen color - Red");
                break;
            case 14:
                outStr("    Chosen color - Yellow");
                break;
            case 8:
                outStr("    Chosen color - Gray");
                break;
            case 7:
                outStr("    Chosen color - White");
                break;
        }
    } else
        if(strncmp(str, "clean", 5) == 0)
            clean();
        else
            if(strncmp(str, "help", 4) == 0){
                outStr("    info - Information about the OS");
                outStr("    clean - Clear the window");
                outStr("    expr - Count expression");
                outStr("    shutdown - Shut the OS down");
            } else
                if(strncmp(str, "expr", 4) == 0)
                    Expression(str + 5);
                else
                    if(strncmp(str, "shutdown", 8) == 0)
                        outw(0x604, 0x2000);
                    else
                        outStr("Error: command not recognized");
}

void symbol(unsigned char scan){
    const unsigned number = (unsigned) scan;
    if(shift || capsLock){
        if(number >= 16 && number <= 50)
            switch(number){
                case 0x1e:
                    outChar('A');
                    break;
                case 0x30:
                    outChar('B');
                    break;
                case 0x2e:
                    outChar('C');
                    break;
                case 0x20:
                    outChar('D');
                    break;
                case 0x12:
                    outChar('E');
                    break;
                case 0x21:
                    outChar('F');
                    break;
                case 0x22:
                    outChar('G');
                    break;
                case 0x23:
                    outChar('H');
                    break;
                case 0x17:
                    outChar('I');
                    break;
                case 0x24:
                    outChar('J');
                    break;
                case 0x25:
                    outChar('K');
                    break;
                case 0x26:
                    outChar('L');
                    break;
                case 0x32:
                    outChar('M');
                    break;
                case 0x31:
                    outChar('N');
                    break;
                case 0x18:
                    outChar('O');
                    break;
                case 0x19:
                    outChar('P');
                    break;
                case 0x10:
                    outChar('Q');
                    break;
                case 0x13:
                    outChar('R');
                    break;
                case 0x1f:
                    outChar('S');
                    break;
                case 0x14:
                    outChar('T');
                    break;
                case 0x16:
                    outChar('U');
                    break;
                case 0x2f:
                    outChar('V');
                    break;
                case 0x11:
                    outChar('W');
                    break;
                case 0x2d:
                    outChar('X');
                    break;
                case 0x15:
                    outChar('Y');
                    break;
                case 0x2c:
                    outChar('Z');
                    break;
            }
        if(number >= 2 && number <= 11)
            switch(number){
                case 2:
                    outChar('!');
                    break;
                case 3:
                    outChar('@');
                    break;
                case 4:
                    outChar('#');
                    break;
                case 5:
                    outChar('$');
                    break;
                case 6:
                    outChar('%');
                    break;
                case 7:
                    outChar('^');
                    break;
                case 8:
                    outChar('&');
                    break;
                case 9:
                    outChar('*');
                    break;
                case 10:
                    outChar('(');
                    break;
                case 11:
                    outChar(')');
                    break;
                default:
                    break;
            }
        if(number == 12)
            outChar('_');
        if(number == 13)
            outChar('+');
        if(number == 53)
            outChar('?');
        if(shift)
            shift = false;
    } else {
        if(number >= 16 && number <= 50)
            switch(number){
                case 0x1e:
                    outChar('a');
                    break;
                case 0x30:
                    outChar('b');
                    break;
                case 0x2e:
                    outChar('c');
                    break;
                case 0x20:
                    outChar('d');
                    break;
                case 0x12:
                    outChar('e');
                    break;
                case 0x21:
                    outChar('f');
                    break;
                case 0x22:
                    outChar('g');
                    break;
                case 0x23:
                    outChar('h');
                    break;
                case 0x17:
                    outChar('i');
                    break;
                case 0x24:
                    outChar('j');
                    break;
                case 0x25:
                    outChar('k');
                    break;
                case 0x26:
                    outChar('l');
                    break;
                case 0x32:
                    outChar('m');
                    break;
                case 0x31:
                    outChar('n');
                    break;
                case 0x18:
                    outChar('o');
                    break;
                case 0x19:
                    outChar('p');
                    break;
                case 0x10:
                    outChar('q');
                    break;
                case 0x13:
                    outChar('r');
                    break;
                case 0x1f:
                    outChar('s');
                    break;
                case 0x14:
                    outChar('t');
                    break;
                case 0x16:
                    outChar('u');
                    break;
                case 0x2f:
                    outChar('v');
                    break;
                case 0x11:
                    outChar('w');
                    break;
                case 0x2d:
                    outChar('x');
                    break;
                case 0x15:
                    outChar('y');
                    break;
                case 0x2c:
                    outChar('z');
                    break;
            }
        if(number >= 2 && number <= 10)
            outChar('1' + number - 2);
        if(number == 11)
            outChar('0');
        if(number == 12)
            outChar('-');
        if(number == 13)
            outChar('=');
        if(number == 53)
            outChar('/');
    }
}

void on_key(unsigned char scan){
    switch(scan){
        case ENTER:
            command();
            break;
        case BACKSPACE:
            delChar();
            break;
        case CAPSLOCK:
            if(!capsLock) {
                capsLock = true;
                if (shift)
                    shift = false;
            } else capsLock = false;
            break;
        case SPACE:
            outChar(' ');
            break;
        case RSHIFT:
            if(capsLock != true)
                shift = true;
            break;
        case LSHIFT:
            if(capsLock != true)
                shift = true;
            break;
        default:
            symbol(scan);
            break;
    }
}

/* ----------------------- Клавиатура -------------------------------- */
void keyb_process_keys(){
// Проверка что буфер PS/2 клавиатуры не пуст (младший бит присутствует)
    if (inb(0x64) & 0x01)
    {
        unsigned char scan_code;
        unsigned char state;
        scan_code = inb(0x60); // Считывание символа с PS/2 клавиатуры
        if (scan_code < 128) // Скан-коды выше 128 - это отпускание клавиши
            on_key(scan_code);
    }
}
void keyb_handler(){
    asm("pusha");
// Обработка поступивших данных
    keyb_process_keys();
// Отправка контроллеру 8259 нотификации о том, что прерывание обработано
    outb(PIC1_PORT, 0x20);
    asm("popa; leave; iret");
}
void keyb_init(){
// Регистрация обработчика прерывания
    intr_reg_handler(0x09, GDT_CS, 0x80 | IDT_TYPE_INTR, keyb_handler); // segm_sel=0x8, P=1, DPL=0, Type=Intr
// Разрешение только прерываний клавиатуры от контроллера 8259
    outb(PIC1_PORT + 1, 0xFF ^ 0x02); // 0xFF - все прерывания, 0x02 - бит IRQ1 (клавиатура).
// Разрешены будут только прерывания, чьи биты установлены в 0
}
/* ------------------------------------------------------------------- */

int setColor(const unsigned arg){
    switch((*(char*)arg)){
        case '1':
            mainColor = 2;
            break;
        case '2':
            mainColor = 1;
            break;
        case '3':
            mainColor = 4;
            break;
        case '4':
            mainColor = 14;
            break;
        case '5':
            mainColor = 8;
            break;
        case '6':
            mainColor = 7;
            break;
        default:
            mainColor = 7;
            outStr("Error with color");
            return -1;
    }
    return 0;
}

extern "C" int kmain()
{
    clean();
    setColor(ARG);
    outStr("Welcome to HelloWorldOS!");

    intr_disable();
    intr_init();
    keyb_init();
    intr_start();
    intr_enable();

    while(1)
        asm("hlt");
    return 0;
}
