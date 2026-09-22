const chinese: Record<string, string> = {
  skip: "跳转到正文",
  whyNav: "为什么选择 GraphFin",
  casesNav: "应用场景",
  reliabilityNav: "可靠性",
  docs: "文档",
  menu: "菜单",
  eyebrow: "原生时序图数据库",
  headlineOne: "关系与观测，",
  headlineTwo: "融为一体。",
  headlineThree: "洞见随之而生。",
  description: "原生支持时间序列的图数据库，为关联金融智能而构建。",
  explore: "探索 GraphFin",
  readDocs: "阅读文档",
  githubSource: "在 GitHub 查看开源代码",
  structure: "结构之中，信号涌现",
  pause: "暂停动画",
  resume: "播放动画",
  introduction: "初识 GRAPHFIN",
  scroll: "沿着关系，发现更多",
  native: "属性图 · 原生时间序列",
  whyTitle: "让关联与历史相遇。",
  whyText:
    "关系告诉你去哪里找，观测告诉你发生了什么。GraphFin 将二者纳入同一数据模型，让顶点与边直接拥有原生时间序列字段。",
  productLink: "了解数据模型",
  casesTitle: "面向关联金融智能。",
  casesText:
    "公司、基金、证券与供应商，以及随时间变化的价格、持仓与风险敞口。投资分析是 GraphFin 这款通用数据库的首个主要应用场景。",
  financeLink: "阅读金融示例",
  casesAside: "关系 → 观测 → 洞见",
  reliabilityTitle: "一个图，一个事务边界。",
  reliabilityText:
    "图记录与时序桶共享同一事务存储。GraphFin 目前处于 Alpha 阶段；恢复、隔离与复制的验证证据及其适用边界均有文档记录。",
  reliabilityLink: "查看验证计划",
  alpha: "0.1.0-ALPHA · 评估阶段",
  tagline: "关系与观测，融为一体。",
  license: "开源 · Apache 2.0",
};

export function setupLanguage(onChange: () => void) {
  const elements = [...document.querySelectorAll<HTMLElement>("[data-i18n]")];
  const english = Object.fromEntries(
    elements.map((el) => [el.dataset.i18n!, el.textContent!]),
  ) as Record<string, string>;
  english.resume = "Resume animation";
  const button = document.querySelector<HTMLButtonElement>("#language")!;
  let language: "en" | "zh" =
    new URL(location.href).searchParams.get("lang") === "zh" ? "zh" : "en";
  const text = (key: string) =>
    (language === "zh" ? chinese[key] : english[key]) || english[key] || key;

  function apply() {
    document.documentElement.lang = language === "zh" ? "zh-CN" : "en";
    for (const element of elements)
      element.textContent = text(element.dataset.i18n!);
    button.innerHTML =
      language === "zh"
        ? '<span class="selected">中</span><span aria-hidden="true"> / </span>EN'
        : '中<span aria-hidden="true"> / </span><span class="selected">EN</span>';
    button.setAttribute(
      "aria-label",
      language === "zh" ? "Switch to English" : "切换为中文",
    );
    document
      .querySelector("nav")!
      .setAttribute(
        "aria-label",
        language === "zh" ? "主导航" : "Main navigation",
      );
    document
      .querySelector(".brand")!
      .setAttribute(
        "aria-label",
        language === "zh" ? "GraphFin 首页" : "GraphFin home",
      );
    document
      .querySelector(".hero-visual")!
      .setAttribute(
        "aria-label",
        language === "zh"
          ? "石墨烯晶格中的连续金色信号"
          : "A graphene lattice carrying a connected gold signal",
      );
    document
      .querySelector("img.hero-fallback")!
      .setAttribute(
        "alt",
        language === "zh"
          ? "GraphFin：G 形碳晶格，相连的金色键承载着信号。"
          : "GraphFin: a carbon lattice shaped like a G, with connected gold bonds carrying a signal.",
      );
    document.title =
      language === "zh"
        ? "GraphFin — 关系与观测，融为一体。"
        : "GraphFin — Relationships and observations, together.";
    document
      .querySelector('meta[name="description"]')!
      .setAttribute("content", text("description"));
    document
      .querySelector('meta[property="og:title"]')!
      .setAttribute("content", document.title);
    document
      .querySelector('meta[property="og:description"]')!
      .setAttribute("content", text("description"));
    onChange();
  }
  button.hidden = false;
  button.addEventListener("click", () => {
    language = language === "en" ? "zh" : "en";
    const url = new URL(location.href);
    if (language === "zh") url.searchParams.set("lang", "zh");
    else url.searchParams.delete("lang");
    history.replaceState(null, "", url);
    apply();
  });
  // URL state makes the selected language shareable; English is always the default.
  apply();
  return { text };
}
