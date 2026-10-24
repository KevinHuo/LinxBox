#include "linx_content.h"

static const linx_content_t s_contents[LINX_CONTENT_COUNT] = {
    [LINX_CONTENT_NONE] = {"正在补充边界测试。", "", ""},
    [LINX_CONTENT_LOGIN_INITIAL] = {
        "登录校验已修复；有一项尚未验证。",
        "登录校验已经调整，补充了空密码和过期令牌的处理。\n\n"
        "主要修改\n"
        "- 空密码会显示明确提示。\n"
        "- 过期令牌会引导用户重新登录。\n"
        "- 登录失败后保留用户输入。\n\n"
        "验证情况\n"
        "演示中的 12 项测试通过；真实服务的错误响应尚未验证。以上数字均为模拟内容。\n\n"
        "需要你确认\n"
        "异常提示是否足够清楚？当前没有发布，等待你的下一步指令。",
        "请确认异常提示文案。"
    },
    [LINX_CONTENT_LOGIN_FOLLOWUP] = {
        "已调整异常提示；真实服务响应尚未验证。",
        "已按你的要求让异常提示更清楚，暂不发布。\n\n"
        "本轮修改\n"
        "- 空密码提示改为：请输入密码。\n"
        "- 令牌过期提示改为：登录已过期，请重新登录。\n\n"
        "验证与边界\n"
        "模拟回归测试通过；真实服务响应尚未验证。本固件没有连接电脑，也没有修改或发布任何真实代码。\n\n"
        "你可以继续告诉我希望怎样调整。",
        "真实服务响应尚未验证。"
    },
    [LINX_CONTENT_PAYMENT_INITIAL] = {
        "超时处理方案待确认：是否自动重试？",
        "支付接口的超时处理需要你确认。\n\n"
        "建议方案\n"
        "请求超时后先查询原交易状态，避免重复支付。确认没有成功后，再按有限次数重试。\n\n"
        "需要你决定\n"
        "最多重试几次？最后仍失败时，是提示用户手动处理，还是继续等待？\n\n"
        "这是固定的演示回复，没有访问真实支付系统。",
        "最多重试几次，失败后怎样处理？"
    },
    [LINX_CONTENT_PAYMENT_FOLLOWUP] = {
        "采用最多两次重试；幂等行为尚未验证。",
        "方案已调整为最多重试两次，仍失败时提示用户手动处理。\n\n"
        "处理顺序\n"
        "1. 超时后查询原交易状态。\n"
        "2. 确认交易未成功后再重试。\n"
        "3. 第二次重试失败后停止，提示用户查看订单。\n\n"
        "尚未验证\n"
        "真实支付服务的幂等行为尚未验证，需要在接入后检查。本轮没有发出任何真实请求。",
        "接入后需要检查服务幂等行为。"
    },
    [LINX_CONTENT_DESIGN_INITIAL] = {
        "采用暖白方案；待确认状态文字大小。",
        "暖白方向已经确定。本轮把任务浏览、结果阅读和继续沟通串成了一条完整流程，并准备了三张界面示例，分别对应任务列表、结果阅读和语音确认。\n\n"
        "整体风格\n\n"
        "背景采用暖白，正文使用接近墨黑的颜色。朱橙只用于 LinX 的 X、当前选择和需要注意的状态。选中的任务同时出现浅色底和左侧标记，这样即使只扫一眼，也能找到自己正在操作哪一条任务。\n\n"
        "任务列表\n\n"
        "每一行首先显示任务名称，下一行显示 Agent 和当前状态。Codex 与 Claude Code 保留各自名称。运行中、等你回复和有新回复分别用文字说明；一个任务结束当前一轮后仍留在列表里，因为你可能还要继续追问。\n\n"
        "怎样阅读结果\n\n"
        "进入任务先看简短结论，再选择阅读完整回复。长回复保留原有段落和列表，使用上下键逐页阅读。如果 Agent 提出了问题，问题会进入摘要，不会藏在很长的执行记录后面。没有提供测试结果时，也不会自行补上“测试通过”。\n\n"
        "怎样查看图片\n\n"
        "这轮附带三张图片。总览用于比较配色和布局，局部视图用于看具体区域；图上的小字如果仍不清楚，可以回到配套说明阅读。你在某张图上发起回复时，系统会记住这张图的身份，“把这张的状态字放大”会发给当前任务，并带着图片上下文。\n\n"
        "怎样继续沟通\n\n"
        "在正文页按 OK 可以继续回复。语音输入结束后先显示文字和目标任务，你确认后才发送。退出转写确认会放弃草稿，不会把未确认的内容发出。等待新的回复时，你仍可以回到任务列表，查看另一条任务。\n\n"
        "需要你决定的事\n\n"
        "当前示例希望保持轻巧的排版。如果你更重视远距离快速辨认，可以把任务状态文字进一步放大。请告诉我你希望优先调整列表中的状态字，还是结果页的正文。\n\n"
        "本轮边界\n\n"
        "这些内容和图片都是演示素材。页面交互完成后还需要在真实屏幕上检查字体、颜色、圆角区域和按键手感；仅靠电脑上的放大预览，不能确认小屏体验已经合适。",
        "希望优先调整状态字，还是结果页正文？"
    },
    [LINX_CONTENT_DESIGN_FOLLOWUP] = {
        "状态文字采用 18 px；真实小屏效果待检查。",
        "已保留暖白方向，把任务状态文字调整为 18 px。\n\n"
        "调整说明\n"
        "选中的任务仍使用浅橙背景和朱橙边标，状态也保留文字说明。此次回复会保留你发起反馈时的原任务、轮次和图片来源。\n\n"
        "下一步\n"
        "还需要在真实小屏上检查文字是否清楚、换行是否自然。这里显示的是固定演示回复，没有重新生成图片；前一轮的三张图片仍然可以查看。",
        "真实小屏上的可读性仍需检查。"
    }
};

static const linx_image_info_t s_images[LINX_IMAGE_COUNT] = {
    {LINX_IMAGE_TASK_LIST, "任务列表", "暖白界面：三条任务和当前状态。"},
    {LINX_IMAGE_RESULT, "结果阅读", "暖白界面：本轮结果和继续沟通。"},
    {LINX_IMAGE_VOICE, "语音确认", "暖白界面：模拟转写和确认发送。"}
};

static const char *const s_users[] = {
    "帮我修复登录校验，补充边界测试，先不要发布。",
    "支付接口超时后应该怎样处理？",
    "设计一个能读完整回复、看图片和继续沟通的 LinX 暖白界面。"
};

static const char *const s_transcripts[] = {
    "把异常提示改得更清楚，先不要发布。",
    "最多重试两次，失败后提示用户手动处理。",
    "保留暖白，把任务状态字放大。",
    "保留暖白，把这张图里的任务状态字放大。"
};

static const char *const s_model_texts[] = {
    "登录校验修复", "支付接口调整", "LinX 暖白界面", "Codex", "Claude Code",
    "运行中", "等你回复", "本轮结束", "模拟发送中", "模拟电脑已收到",
    "已排队", "模拟 Agent 已接收", "模拟 Agent 运行中", "模拟发送失败", "尚未发送"
};

const linx_content_t *linx_content_get(linx_content_id_t id)
{
    if ((unsigned)id >= LINX_CONTENT_COUNT) {
        return NULL;
    }
    return &s_contents[id];
}

const linx_image_info_t *linx_content_image(uint32_t image_id)
{
    for (size_t i = 0; i < LINX_IMAGE_COUNT; ++i) {
        if (s_images[i].id == image_id) {
            return &s_images[i];
        }
    }
    return NULL;
}

const char *linx_content_initial_user(uint32_t task_id)
{
    return task_id >= 1U && task_id <= 3U ? s_users[task_id - 1U] : "";
}

const char *linx_content_transcript(uint32_t task_id, uint32_t image_id)
{
    if (task_id == 3U && image_id != 0U) {
        return s_transcripts[3];
    }
    return task_id >= 1U && task_id <= 3U ? s_transcripts[task_id - 1U] : "";
}

size_t linx_content_text_count(void)
{
    return LINX_CONTENT_COUNT * 3U + LINX_IMAGE_COUNT * 2U +
           sizeof(s_users) / sizeof(s_users[0]) +
           sizeof(s_transcripts) / sizeof(s_transcripts[0]) +
           sizeof(s_model_texts) / sizeof(s_model_texts[0]);
}

const char *linx_content_text_at(size_t index)
{
    size_t count = LINX_CONTENT_COUNT * 3U;
    if (index < count) {
        const linx_content_t *content = &s_contents[index / 3U];
        switch (index % 3U) {
        case 0: return content->summary;
        case 1: return content->body;
        default: return content->question;
        }
    }
    index -= count;
    count = LINX_IMAGE_COUNT * 2U;
    if (index < count) {
        return index % 2U == 0U ? s_images[index / 2U].name : s_images[index / 2U].description;
    }
    index -= count;
    count = sizeof(s_users) / sizeof(s_users[0]);
    if (index < count) {
        return s_users[index];
    }
    index -= count;
    count = sizeof(s_transcripts) / sizeof(s_transcripts[0]);
    if (index < count) {
        return s_transcripts[index];
    }
    index -= count;
    count = sizeof(s_model_texts) / sizeof(s_model_texts[0]);
    return index < count ? s_model_texts[index] : NULL;
}
