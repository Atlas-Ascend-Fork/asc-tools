# 输出模板 — 描述性 Markdown

diagnose 报告的渲染骨架，字段及渲染行为统一见 `references/templates/output_spec.md`，JSON 示例见 `references/templates/output_json.md`。`{{path}}` 取值、`{% for %}` 迭代、`{% if %}` 条件。

## 骨架

````markdown
# AI Core 异常检测报告

**环境**：{{env.hardware}} · {{env.cann}} · {{env.skill}} · {{env.runtime}}

{% if not candidates %}
当前证据未形成已知异常候选；这不代表源码已验证安全。
{% endif %}

{% for c in candidates %}
---

## {{c.id}}

{% if c.category == "static_preventive" %}*(静态预防 · {{c.nature}})*{% endif %}

**修改原因**：{{c.fix.reason}}

{% for cb in c.fix.code_blocks %}
### {{cb.title}}

**违规代码：**
```{{cb.language}}
{{cb.before}}
```

**修复代码：**
```{{cb.language}}
{{cb.after}}
```
{% endfor %}

{% endfor %}
````
