const chinese: Record<string, string> = {
  skip: "跳转到正文",
  whyNav: "能做什么",
  casesNav: "示例",
  reliabilityNav: "项目进展",
  docs: "文档",
  menu: "菜单",
  eyebrow: "开源图数据库",
  headlineOne: "关系和变化，",
  headlineTwo: "",
  headlineThree: "一库掌握",
  description:
    "把公司关系、股票价格和基金持仓放在一起。先找到相关对象，再看它们的数据怎样变化。",
  explore: "了解它怎么用",
  readDocs: "阅读文档",
  githubSource: "在 GitHub 查看源码",
  structure: "关系 + 时间序列",
  introduction: "产品介绍",
  scroll: "继续了解",
  native: "关系查询 · 历史数据",
  whyTitle: "相关的数据，放在一起。",
  whyText:
    "股票有自己的价格记录，基金和股票之间的持有关系有自己的持仓记录。沿着关系找到对象后，就能继续查询对应的历史数据。",
  productLink: "看看数据怎么存",
  conceptLabel: "基金 → 股票 · 历史价格",
  casesTitle: "从这些问题开始。",
  casesText: "示例整理中。每个示例会包含数据、查询步骤和结果。",
  financeLink: "查看已有的金融示例代码",
  casesAside: "数据 · 查询 · 结果",
  financeCategory: "01 / 投资研究",
  financeTitle: "基金持仓怎么变了？",
  financeText: "从基金找到股票，查看一段时间内的持仓记录。",
  equipmentCategory: "02 / 设备监测",
  equipmentTitle: "这台设备的温度怎样变化？",
  equipmentText: "找到设备上的传感器，读取过去一小时的温度记录。",
  supplyCategory: "03 / 供应链",
  supplyTitle: "交货时间变长了吗？",
  supplyText: "找到一家公司的供应商，对比记录中的交货时间。",
  examplePending: "示例待补充",
  exampleContents: "示例数据 · 查询步骤 · 结果",
  reliabilityTitle: "还在开发，欢迎试用。",
  reliabilityText:
    "当前版本适合在测试环境中试用。重启、备份恢复等测试已有记录，更多稳定性测试还在继续。",
  reliabilityLink: "查看测试进展",
  alpha: "0.1.0-ALPHA · 适合试用评估",
  tagline: "关系和变化，一库掌握",
  license: "开源 · Apache 2.0",
};

export function setupLanguage(onChange: () => void) {
  const elements = [...document.querySelectorAll<HTMLElement>("[data-i18n]")];
  const english = Object.fromEntries(
    elements.map((el) => [el.dataset.i18n!, el.textContent!]),
  ) as Record<string, string>;
  const button = document.querySelector<HTMLButtonElement>("#language")!;
  let language: "en" | "zh" =
    new URL(location.href).searchParams.get("lang") === "zh" ? "zh" : "en";
  const text = (key: string) =>
    (language === "zh" ? chinese[key] : english[key]) ?? english[key] ?? key;

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
          ? "金色光点沿着 G 形金属网格移动"
          : "A metal G with a gold pulse moving through it",
      );
    document
      .querySelector("img.hero-fallback")!
      .setAttribute(
        "alt",
        language === "zh"
          ? "GraphFin：G 形金属网格，金色光点沿网格移动。"
          : "GraphFin: a metal G with a gold pulse moving through it.",
      );
    document.title =
      language === "zh"
        ? "GraphFin — 关系和变化，一库掌握"
        : "GraphFin — Query connections and track changes.";
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
