-- ==============================================================================
-- سكريبت عداد الوقت الذكي لـ HK Stream (Smart Recording & Streaming Timer)
-- ==============================================================================
-- 🏛️ القاعدة الأساسية الصريحة:
-- البث له الأولوية المطلقة. ما دام البث المباشر يعمل، فإن حالة التسجيل كلها غير معتبرة
-- بالنسبة للعداد، سواء بدأ التسجيل أو توقف أو دخل Pause أو خرج منه.
-- يدعم السكربت تلقائياً جميع المصادر الأساسية والمعزولة والمستنسخة لبيئات العمل (Workspaces)
-- بحيث يتم تحديث "عداد الوقت" وجميع نسخه في كافة المشاهد في آنٍ واحد ودون أي تجميد.
-- ------------------------------------------------------------------------------
-- 💻 المنطق البرمجي الدقيق المطبق:
-- إذا كان البث يعمل:
--     العداد يعمل دائمًا
-- وإلا إذا كان التسجيل يعمل:
--     إذا كان التسجيل في Pause:
--         العداد متوقف مؤقتًا (يتجمد)
--     وإلا:
--         العداد يعمل
-- وإلا:
--     العداد متوقف
-- ==============================================================================

local obs = obslua

-- ==============================================================================
-- متغيرات الحالة والتوقيت (Monotonic State Machine)
-- ==============================================================================
-- الحالات الممكنة: "STOPPED", "RUNNING", "PAUSED"
local timer_state = "STOPPED"
local accumulated_seconds = 0
local segment_start_ns = 0
local last_displayed_seconds = -1
local timer_active = false
local test_mode = false

local is_recording_event_active = false
local is_streaming_event_active = false

-- متغيرات الإعدادات (Settings)
local source_name = "عداد الوقت"
local format_style = "hh_mm_ss"
local prefix_text = ""
local apply_font = false
local current_font_obj = nil
local apply_color = false
local text_color = 0xFFFFFFFF -- أبيض افتراضي (0xAABBGGRR)
local enable_outline = false
local outline_color = 0xFF000000 -- أسود افتراضي
local outline_size = 4
local reset_on_stop = true

-- ==============================================================================
-- استخراج الاسم المجرد ومطابقة المصادر (Workspace Clones & Aliases)
-- ==============================================================================

-- استخراج الاسم الأساسي للمصدر (إزالة أي لواحق مثل [WS:...] أو [16:9] أو اسم المشهد)
local function get_base_source_name(name)
    if not name or name == "" then return "" end
    local base = name:match("^(.-)%s*%[")
    if base and base ~= "" then
        return base
    end
    return name
end

-- فحص هل المصدر الحالي يطابق المصدر المستهدف (سواء كان الأصل أو نسخة معزولة للمشهد)
local function is_matching_source(src_name)
    if not src_name or src_name == "" then return false end

    -- 1. مطابقة تامة بالاسم المحدد
    if source_name and source_name ~= "" and src_name == source_name then
        return true
    end

    local target_base = get_base_source_name(source_name)
    local cur_base = get_base_source_name(src_name)

    -- 2. مطابقة الاسم المجرد
    if target_base ~= "" and cur_base ~= "" and target_base == cur_base then
        return true
    end

    -- 3. مطابقة اللواحق الصريحة للمصادر المستنسخة: يبدأ بـ "source_name ["
    if target_base ~= "" then
        local prefix = target_base .. " ["
        if src_name:sub(1, #prefix) == prefix then
            return true
        end
    end

    -- 4. دعم ذكي لكلمة "عداد": إذا كان الهدف عداداً، طابق أي مصدر نصي يحمل "عداد"
    if (target_base == "" or string.find(target_base, "عداد")) and string.find(src_name, "عداد") then
        return true
    end

    return false
end

-- ==============================================================================
-- حساب الزمن بدقة عالية (Monotonic Elapsed Time)
-- ==============================================================================

local function get_current_seconds()
    if timer_state == "RUNNING" and segment_start_ns > 0 then
        local now_ns = obs.os_gettime_ns()
        local delta_s = (now_ns - segment_start_ns) / 1000000000.0
        return math.max(0, math.floor(accumulated_seconds + delta_s))
    else
        return math.max(0, math.floor(accumulated_seconds))
    end
end

-- تحويل الثواني إلى نص منسق (ساعات:دقائق:ثواني)
local function format_time(seconds)
    seconds = seconds or 0
    local h = math.floor(seconds / 3600)
    local m = math.floor((seconds % 3600) / 60)
    local s = seconds % 60

    local time_str = ""
    if format_style == "hh_mm_ss" then
        time_str = string.format("%02d:%02d:%02d", h, m, s)
    elseif format_style == "mm_ss" then
        local total_m = math.floor(seconds / 60)
        time_str = string.format("%02d:%02d:%02d", total_m, s)
    elseif format_style == "h_mm_ss" then
        time_str = string.format("%d:%02d:%02d", h, m, s)
    else
        time_str = string.format("%02d:%02d:%02d", h, m, s)
    end

    if prefix_text and prefix_text ~= "" then
        return prefix_text .. " " .. time_str
    else
        return time_str
    end
end

-- ==============================================================================
-- تحديث مصدر النص في HK مع التمييز بين GDI+ و FreeType 2 ودعم كل النسخ
-- ==============================================================================

local function update_single_source(source, formatted_text, apply_styling)
    if source == nil then return end

    local source_id = obs.obs_source_get_unversioned_id(source)
    local is_gdi = (source_id == "text_gdiplus" or source_id == "text_gdiplus_v2")
    local is_ft2 = (source_id == "text_ft2_source" or source_id == "text_ft2_source_v2")

    if not is_gdi and not is_ft2 then
        return
    end

    local settings = obs.obs_data_create()
    obs.obs_data_set_string(settings, "text", formatted_text)

    if apply_styling then
        if is_gdi then
            obs.obs_data_set_bool(settings, "outline", enable_outline)
            obs.obs_data_set_int(settings, "outline_color", outline_color)
            obs.obs_data_set_int(settings, "outline_size", outline_size)
            if apply_color then
                obs.obs_data_set_int(settings, "color", text_color)
            end
        elseif is_ft2 then
            obs.obs_data_set_bool(settings, "outline", enable_outline)
            if apply_color then
                obs.obs_data_set_int(settings, "color1", text_color)
                obs.obs_data_set_int(settings, "color2", text_color)
            end
        end

        if apply_font and current_font_obj ~= nil then
            obs.obs_data_set_obj(settings, "font", current_font_obj)
        end
    end

    obs.obs_source_update(source, settings)
    obs.obs_data_release(settings)
end

local function update_text_source(apply_styling)
    local cur_seconds = get_current_seconds()
    local formatted = format_time(cur_seconds)

    local sources = obs.obs_enum_sources()
    if sources ~= nil then
        for _, source in ipairs(sources) do
            local name = obs.obs_source_get_name(source)
            if is_matching_source(name) then
                update_single_source(source, formatted, apply_styling)
            end
        end
        obs.source_list_release(sources)
    end
end

-- نبضة المؤقت (تُستدعى كل 250ms لفحص دقيق واستجابة فورية)
local function on_tick()
    if timer_state == "RUNNING" then
        local cur_sec = get_current_seconds()
        if cur_sec ~= last_displayed_seconds then
            last_displayed_seconds = cur_sec
            update_text_source(false)
        end
    end
end

local function start_tick_timer()
    if not timer_active then
        obs.timer_add(on_tick, 250)
        timer_active = true
    end
end

local function stop_tick_timer()
    if timer_active then
        obs.timer_remove(on_tick)
        timer_active = false
    end
end

-- التحويل بين حالات العداد
local function set_timer_state(new_state)
    if timer_state == new_state then
        return
    end

    local now_ns = obs.os_gettime_ns()

    if new_state == "RUNNING" then
        if timer_state == "STOPPED" then
            if reset_on_stop then
                accumulated_seconds = 0
            end
            segment_start_ns = now_ns
        elseif timer_state == "PAUSED" then
            segment_start_ns = now_ns
        end
        timer_state = "RUNNING"
        last_displayed_seconds = get_current_seconds()
        start_tick_timer()

    elseif new_state == "PAUSED" then
        if timer_state == "RUNNING" and segment_start_ns > 0 then
            local delta_s = (now_ns - segment_start_ns) / 1000000000.0
            accumulated_seconds = accumulated_seconds + delta_s
            segment_start_ns = 0
        end
        timer_state = "PAUSED"
        last_displayed_seconds = get_current_seconds()

    elseif new_state == "STOPPED" then
        if timer_state == "RUNNING" and segment_start_ns > 0 then
            local delta_s = (now_ns - segment_start_ns) / 1000000000.0
            accumulated_seconds = accumulated_seconds + delta_s
            segment_start_ns = 0
        end
        timer_state = "STOPPED"
        stop_tick_timer()
        if reset_on_stop then
            accumulated_seconds = 0
        end
        last_displayed_seconds = get_current_seconds()
    end

    update_text_source(false)
end

-- ==============================================================================
-- تقييم حالة العداد (OBS Engine is Single Source of Truth)
-- ==============================================================================

local function evaluate_state()
    local live_streaming = obs.obs_frontend_streaming_active() or is_streaming_event_active
    local live_recording = obs.obs_frontend_recording_active() or is_recording_event_active
    local live_paused = obs.obs_frontend_recording_paused()

    -- إذا بدأ بث أو تسجيل حقيقي، يتم إلغاء وضع الاختبار فوراً
    if live_streaming or live_recording then
        test_mode = false
    end

    if test_mode then
        return
    end

    -- تطبيق القاعدة الأساسية:
    if live_streaming then
        -- 1. البث يعمل: له الأولوية المطلقة
        set_timer_state("RUNNING")
        obs.script_log(obs.LOG_INFO, "[Smart Timer] بث نشط (أولوية مطلقة) -> العداد يعمل.")

    elseif live_recording then
        -- 2. لا يوجد بث، والتسجيل يعمل: الاعتماد الكامل على التسجيل
        if live_paused then
            set_timer_state("PAUSED")
            obs.script_log(obs.LOG_INFO, "[Smart Timer] تسجيل فقط في وضع Pause -> العداد متجمد مؤقتاً.")
        else
            set_timer_state("RUNNING")
            obs.script_log(obs.LOG_INFO, "[Smart Timer] تسجيل فقط نشط -> العداد يعمل.")
        end

    else
        -- 3. لا يوجد بث ولا تسجيل: توقف تام
        set_timer_state("STOPPED")
        obs.script_log(obs.LOG_INFO, "[Smart Timer] توقف تام (لا بث ولا تسجيل) -> العداد متوقف.")
    end
end

-- ==============================================================================
-- معالجة أحداث OBS (Frontend Events Callback)
-- ==============================================================================

local function on_frontend_event(event)
    if event == obs.OBS_FRONTEND_EVENT_STREAMING_STARTED then
        is_streaming_event_active = true
        evaluate_state()
    elseif event == obs.OBS_FRONTEND_EVENT_STREAMING_STOPPED then
        is_streaming_event_active = false
        evaluate_state()
    elseif event == obs.OBS_FRONTEND_EVENT_RECORDING_STARTED then
        is_recording_event_active = true
        evaluate_state()
    elseif event == obs.OBS_FRONTEND_EVENT_RECORDING_STOPPED then
        is_recording_event_active = false
        evaluate_state()
    elseif event == obs.OBS_FRONTEND_EVENT_RECORDING_PAUSED or
           event == obs.OBS_FRONTEND_EVENT_RECORDING_UNPAUSED then
        evaluate_state()
    elseif event == obs.OBS_FRONTEND_EVENT_SCENE_CHANGED then
        evaluate_state()
    end
end

-- ==============================================================================
-- تعبئة قائمة مصادر النصوص (مع إزالة التكرار)
-- ==============================================================================

local function populate_text_sources(props)
    local p_source = obs.obs_properties_get(props, "source_name")
    if p_source ~= nil then
        obs.obs_property_list_clear(p_source)
        obs.obs_property_list_add_string(p_source, "(اختر مصدر النص / None)", "")

        local seen = {}
        local sources = obs.obs_enum_sources()
        if sources ~= nil then
            for _, source in ipairs(sources) do
                local source_id = obs.obs_source_get_unversioned_id(source)
                if source_id == "text_gdiplus" or source_id == "text_ft2_source" or source_id == "text_gdiplus_v2" then
                    local name = obs.obs_source_get_name(source)
                    local base = get_base_source_name(name)
                    if base ~= "" and not seen[base] then
                        seen[base] = true
                        obs.obs_property_list_add_string(p_source, base, base)
                    end
                end
            end
            obs.source_list_release(sources)
        end
    end
end

-- ==============================================================================
-- أزرار التحكم اليدوي والتجربة
-- ==============================================================================

local function on_btn_toggle_test(props, prop)
    local live_streaming = obs.obs_frontend_streaming_active()
    local live_recording = obs.obs_frontend_recording_active()

    if live_streaming or live_recording then
        obs.script_log(obs.LOG_WARNING, "[Smart Timer] تنبيه: لا يمكن تشغيل وضع الاختبار أثناء وجود بث أو تسجيل حقيقي قيد التشغيل.")
        return false
    end

    if test_mode then
        test_mode = false
        set_timer_state("STOPPED")
        obs.script_log(obs.LOG_INFO, "[Smart Timer] تم إيقاف وضع الاختبار وعودة العداد للجاهزية.")
    else
        test_mode = true
        set_timer_state("RUNNING")
        obs.script_log(obs.LOG_INFO, "[Smart Timer] تم تشغيل وضع الاختبار لمعاينة المظهر.")
    end

    return true
end

local function on_btn_reset(props, prop)
    accumulated_seconds = 0
    if timer_state == "RUNNING" then
        segment_start_ns = obs.os_gettime_ns()
    else
        segment_start_ns = 0
    end
    last_displayed_seconds = 0
    update_text_source(false)
    return true
end

local function on_btn_refresh(props, prop)
    populate_text_sources(props)
    return true
end

-- ==============================================================================
-- واجهة إعدادات السكربت (Script Properties)
-- ==============================================================================

function script_description()
    return [[
<h2>⏱️ سكريبت عداد الوقت الذكي (Smart Timer) - الإصدار المحسن لـ HK Stream</h2>
<p><b>القاعدة الأساسية:</b> البث له الأولوية المطلقة. ما دام البث يعمل، فحالة التسجيل غير معتبرة بالنسبة للعداد.</p>
<p>يدعم السكربت تلقائياً بيئات العمل (Workspaces) ويقوم بتحديث العداد على كافة المشاهد والنسخ المعزولة بالتوازي.</p>
<table border="1" cellpadding="5" style="border-collapse: collapse; font-size: 13px;">
  <tr bgcolor="#222"><th>الحالة</th><th>سلوك العداد</th></tr>
  <tr><td>بث يعمل بمفرده</td><td>يعمل دائماً</td></tr>
  <tr><td>بث + تسجيل</td><td>يعمل بحسب البث فقط</td></tr>
  <tr><td>بث + تسجيل Pause</td><td>يعمل ولا يتأثر إطلاقاً</td></tr>
  <tr><td>توقف التسجيل والبث مستمر</td><td>يستمر بالعمل دون توقف</td></tr>
  <tr><td>توقف البث والتسجيل مستمر</td><td>ينتقل للاعتماد على التسجيل ويستمر</td></tr>
  <tr><td>تسجيل فقط</td><td>يعمل</td></tr>
  <tr><td>تسجيل فقط + Pause</td><td>يتجمد العداد مؤقتاً</td></tr>
  <tr><td>توقف التسجيل ولا يوجد بث</td><td>يتوقف العداد كلياً</td></tr>
</table>
<hr>
]]
end

function script_properties()
    local props = obs.obs_properties_create()

    -- 1. اختيار مصدر النص
    local p_source = obs.obs_properties_add_list(
        props,
        "source_name",
        "📌 مصدر النص (Text Source)",
        obs.OBS_COMBO_TYPE_EDITABLE,
        obs.OBS_COMBO_FORMAT_STRING
    )
    populate_text_sources(props)

    obs.obs_properties_add_button(props, "btn_refresh", "🔄 تحديث قائمة مصادر النصوص", on_btn_refresh)

    -- 2. نمط تنسيق الوقت
    local p_format = obs.obs_properties_add_list(
        props,
        "format_style",
        "🕒 نمط عرض الوقت",
        obs.OBS_COMBO_TYPE_LIST,
        obs.OBS_COMBO_FORMAT_STRING
    )
    obs.obs_property_list_add_string(p_format, "ساعات:دقائق:ثواني (00:00:00)", "hh_mm_ss")
    obs.obs_property_list_add_string(p_format, "دقائق:ثواني (00:00)", "mm_ss")
    obs.obs_property_list_add_string(p_format, "ساعة:دقيقة:ثانية بدون أصفار مسبقة (0:00:00)", "h_mm_ss")

    obs.obs_properties_add_text(props, "prefix_text", "🏷️ بادئة نصية (اختياري، مثلاً REC)", obs.OBS_TEXT_DEFAULT)

    -- 3. تخصيص الخط
    obs.obs_properties_add_bool(props, "apply_font", "✏️ تطبيق الخط من السكربت على المصدر")
    obs.obs_properties_add_font(props, "font_setting", "🔤 نوع وحجم ونمط الخط")

    -- 4. تخصيص لون النص
    obs.obs_properties_add_bool(props, "apply_color", "🎨 تطبيق لون النص من السكربت")
    obs.obs_properties_add_color(props, "text_color", "🎨 لون النص")

    -- 5. إطار الكتابة (Outline / Border)
    obs.obs_properties_add_bool(props, "enable_outline", "🖼️ تفعيل إطار للكتابة (Outline / Border)")
    obs.obs_properties_add_color(props, "outline_color", "🖌️ لون الإطار (خاص بـ GDI+)")
    obs.obs_properties_add_int_slider(props, "outline_size", "📏 سماكة الإطار (خاص بـ GDI+)", 1, 30, 1)

    -- 6. سلوك العداد
    obs.obs_properties_add_bool(props, "reset_on_stop", "🔁 تصفير العداد إلى 00:00:00 عند التوقف التام")

    -- 7. أزرار التجربة السريعة
    obs.obs_properties_add_button(props, "btn_toggle_test", "▶ / ⏸ تشغيل / إيقاف تجريبي (Test)", on_btn_toggle_test)
    obs.obs_properties_add_button(props, "btn_reset", "🔄 تصفير العداد (Reset)", on_btn_reset)

    return props
end

function script_defaults(settings)
    obs.obs_data_set_default_string(settings, "source_name", "عداد الوقت")
    obs.obs_data_set_default_string(settings, "format_style", "hh_mm_ss")
    obs.obs_data_set_default_string(settings, "prefix_text", "")
    obs.obs_data_set_default_bool(settings, "apply_font", false)
    obs.obs_data_set_default_bool(settings, "apply_color", false)
    obs.obs_data_set_default_int(settings, "text_color", 0xFFFFFFFF)
    obs.obs_data_set_default_bool(settings, "enable_outline", false)
    obs.obs_data_set_default_int(settings, "outline_color", 0xFF000000)
    obs.obs_data_set_default_int(settings, "outline_size", 4)
    obs.obs_data_set_default_bool(settings, "reset_on_stop", true)
end

function script_update(settings)
    source_name = obs.obs_data_get_string(settings, "source_name")
    if source_name == "" or source_name == "(اختر مصدر النص / None)" then
        source_name = "عداد الوقت"
    end
    format_style = obs.obs_data_get_string(settings, "format_style")
    prefix_text = obs.obs_data_get_string(settings, "prefix_text")
    apply_font = obs.obs_data_get_bool(settings, "apply_font")
    apply_color = obs.obs_data_get_bool(settings, "apply_color")
    text_color = obs.obs_data_get_int(settings, "text_color")
    enable_outline = obs.obs_data_get_bool(settings, "enable_outline")
    outline_color = obs.obs_data_get_int(settings, "outline_color")
    outline_size = obs.obs_data_get_int(settings, "outline_size")
    reset_on_stop = obs.obs_data_get_bool(settings, "reset_on_stop")

    if current_font_obj ~= nil then
        obs.obs_data_release(current_font_obj)
        current_font_obj = nil
    end

    local font_data = obs.obs_data_get_obj(settings, "font_setting")
    if font_data ~= nil then
        current_font_obj = font_data
    end

    update_text_source(true)
end

function script_load(settings)
    obs.obs_frontend_add_event_callback(on_frontend_event)
    script_update(settings)
    evaluate_state()
    update_text_source(true)
    obs.script_log(obs.LOG_INFO, "[Smart Timer] تم تحميل سكريبت العداد الذكي المحدث بنجاح.")
end

function script_unload()
    stop_tick_timer()
    obs.obs_frontend_remove_event_callback(on_frontend_event)

    if current_font_obj ~= nil then
        obs.obs_data_release(current_font_obj)
        current_font_obj = nil
    end

    obs.script_log(obs.LOG_INFO, "[Smart Timer] تم تفريغ سكريبت العداد الذكي بأمان.")
end
