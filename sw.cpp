void build(Solution &s)
{
    auto &bot = s.addTarget<ExecutableTarget>("mechanoids_quest");
    bot += cpp23;
    bot.PackageDefinitions = true;
    bot += "mechanoids_quest.cpp";
    bot += "SW_EXECUTABLE"_def;
    bot +=
        "pub.egorpugin.primitives.http"_dep,
        "pub.egorpugin.primitives.sw.main"_dep,
        "org.sw.demo.fmt"_dep,
        "org.sw.demo.lua"_dep,
        "org.sw.demo.tgbot"_dep
        ;
    if (!bot.getBuildSettings().TargetOS.is(OSType::Windows))
        bot.LinkOptions.push_back("-Wl,--export-dynamic");
}
