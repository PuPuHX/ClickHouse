#include <Parsers/IParserBase.h>


namespace DB
{

bool IParserBase::parse(Pos & pos, ASTPtr & node, Expected & expected)
{
    expected.add(pos, getName());
    auto msg = fmt::format(
        "depth:{} <{}>({}) parser:{} description:{}",
        pos.depth,
        getTokenName(pos->type),
        std::string_view(pos->begin, pos->end - pos->begin),
        this->getParserName(),
        getName());
    auto ret = wrapParseImpl(
        pos,
        IncreaseDepthTag{},
        [&]
        {
            bool res = parseImpl(pos, node, expected);
            if (!res)
                node = nullptr;
            return res;
        });
    if (ret)
    {
        std::cout << fmt::format("{} cur pos:{}\n", msg, std::string_view(pos->begin, pos->end - pos->begin));
    }
    return ret;
}

}
