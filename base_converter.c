#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <notification/notification_messages.h>

#define BASES_COUNT 4

// Системы счисления: основание, сколько цифр показывать, подпись
static const uint8_t bases[BASES_COUNT] = {2, 8, 10, 16};
static const uint8_t widths[BASES_COUNT] = {16, 6, 5, 4};
static const char* labels[BASES_COUNT] = {"BIN", "OCT", "DEC", "HEX"};

// Всё состояние приложения в одной структуре
typedef struct {
    uint16_t value; // само число (0..65535)
    uint8_t base_index; // активная система (индекс в bases)
    uint8_t cursor; // позиция курсора справа налево, 0 = младший разряд
    FuriMutex* mutex; // защищает состояние: рисование идёт в другом потоке
} AppState;

// Перевод числа в строку в системе base с ведущими нулями до width символов.
// Цифры получаются остатками от деления, поэтому заполняем строку с конца.
static void to_base(uint32_t value, uint8_t base, char* out, uint8_t width) {
    const char* digits = "0123456789ABCDEF";
    out[width] = '\0';
    for(int8_t i = width - 1; i >= 0; i--) {
        out[i] = digits[value % base];
        value /= base;
    }
}

// "Вес" разряда: base в степени pos
static uint32_t place_value(uint8_t base, uint8_t pos) {
    uint32_t result = 1;
    for(uint8_t i = 0; i < pos; i++) {
        result *= base;
    }
    return result;
}

// Меняет цифру под курсором на delta (+1 или -1) с заворотом 9->0, F->0 и т.п.
// Возвращает false, если результат не влезает в uint16_t (тогда число не меняется).
static bool change_digit(AppState* state, int8_t delta) {
    uint8_t base = bases[state->base_index];
    uint32_t place = place_value(base, state->cursor);
    int32_t digit = (state->value / place) % base;
    int32_t new_digit = (digit + delta + base) % base;

    uint32_t new_value = state->value - digit * place + new_digit * place;
    if(new_value > UINT16_MAX) {
        return false;
    }
    state->value = (uint16_t)new_value;
    return true;
}

// Рисование экрана. Вызывается GUI-потоком каждый раз после view_port_update().
static void draw_callback(Canvas* canvas, void* ctx) {
    AppState* state = ctx;
    furi_mutex_acquire(state->mutex, FuriWaitForever);

    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str_aligned(canvas, 64, 0, AlignCenter, AlignTop, "Base Converter");

    canvas_set_font(canvas, FontKeyboard); // моноширинный шрифт для цифр
    uint8_t char_w = canvas_glyph_width(canvas, '0');
    char buf[17];

    for(uint8_t i = 0; i < BASES_COUNT; i++) {
        uint8_t y = 22 + i * 11; // базовая линия строки
        bool active = (i == state->base_index);

        // Подпись слева
        canvas_set_font(canvas, FontSecondary);
        canvas_draw_str(canvas, 0, y, active ? ">" : "");
        canvas_draw_str(canvas, 6, y, labels[i]);

        // Число, выровненное по правому краю
        canvas_set_font(canvas, FontKeyboard);
        to_base(state->value, bases[i], buf, widths[i]);
        uint8_t x0 = 128 - widths[i] * char_w;
        canvas_draw_str(canvas, x0, y, buf);

        // Курсор: чёрный прямоугольник и белая цифра поверх
        if(active) {
            uint8_t idx = widths[i] - 1 - state->cursor;
            uint8_t cx = x0 + idx * char_w;
            char ch[2] = {buf[idx], '\0'};
            canvas_draw_box(canvas, cx, y - 9, char_w, 11);
            canvas_set_color(canvas, ColorWhite);
            canvas_draw_str(canvas, cx, y, ch);
            canvas_set_color(canvas, ColorBlack);
        }
    }

    furi_mutex_release(state->mutex);
}

// Нажатия кнопок. Здесь ничего не обрабатываем, только кладём событие в очередь.
static void input_callback(InputEvent* event, void* ctx) {
    FuriMessageQueue* queue = ctx;
    furi_message_queue_put(queue, event, FuriWaitForever);
}

int32_t base_converter_app(void* p) {
    UNUSED(p);

    AppState* state = malloc(sizeof(AppState));
    state->value = 0;
    state->base_index = 2; // начинаем с DEC
    state->cursor = 0;
    state->mutex = furi_mutex_alloc(FuriMutexTypeNormal);

    FuriMessageQueue* queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    ViewPort* view_port = view_port_alloc();
    view_port_draw_callback_set(view_port, draw_callback, state);
    view_port_input_callback_set(view_port, input_callback, queue);

    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view_port, GuiLayerFullscreen);
    NotificationApp* notifications = furi_record_open(RECORD_NOTIFICATION);

    InputEvent event;
    bool running = true;
    while(running && furi_message_queue_get(queue, &event, FuriWaitForever) == FuriStatusOk) {
        // Нажатие и отпускание игнорируем, работаем с Short / Long / Repeat
        if(event.type == InputTypePress || event.type == InputTypeRelease) {
            continue;
        }

        bool overflow = false;
        furi_mutex_acquire(state->mutex, FuriWaitForever);

        switch(event.key) {
        case InputKeyBack:
            if(event.type == InputTypeShort) running = false;
            break;

        case InputKeyOk:
            if(event.type == InputTypeShort) {
                // Следующая система; курсор не должен вылезти за длину строки
                state->base_index = (state->base_index + 1) % BASES_COUNT;
                if(state->cursor >= widths[state->base_index]) {
                    state->cursor = widths[state->base_index] - 1;
                }
            } else if(event.type == InputTypeLong) {
                state->value = 0; // долгое нажатие OK — сброс
            }
            break;

        case InputKeyUp:
        case InputKeyDown:
            if(event.type == InputTypeShort || event.type == InputTypeRepeat) {
                int8_t delta = (event.key == InputKeyUp) ? 1 : -1;
                overflow = !change_digit(state, delta);
            }
            break;

        case InputKeyLeft:
            if(event.type == InputTypeShort && state->cursor < widths[state->base_index] - 1) {
                state->cursor++;
            }
            break;

        case InputKeyRight:
            if(event.type == InputTypeShort && state->cursor > 0) {
                state->cursor--;
            }
            break;

        default:
            break;
        }

        furi_mutex_release(state->mutex);

        if(overflow) {
            notification_message(notifications, &sequence_single_vibro);
        }
        view_port_update(view_port);
    }

    // Освобождаем всё в обратном порядке
    view_port_enabled_set(view_port, false);
    gui_remove_view_port(gui, view_port);
    view_port_free(view_port);
    furi_record_close(RECORD_NOTIFICATION);
    furi_record_close(RECORD_GUI);
    furi_message_queue_free(queue);
    furi_mutex_free(state->mutex);
    free(state);

    return 0;
}
