void build(Solution &s)
{
    auto &bot = s.addTarget<ExecutableTarget>("mechanoids_quest");
    bot.CPPVersion = CPPLanguageStandard::CPP17;
    bot.PackageDefinitions = true;
    bot += "mechanoids_quest.cpp";
    bot += "SW_EXECUTABLE"_def;
    bot +=
        "pub.egorpugin.primitives.filesystem-master"_dep,
        "pub.egorpugin.primitives.templates-master"_dep,
        "pub.egorpugin.primitives.sw.main-master"_dep,
        "org.sw.demo.lua-5"_dep,
        "org.sw.demo.fmt-5"_dep,
        "org.sw.demo.reo7sp.tgbot-master"_dep
    ;
}
