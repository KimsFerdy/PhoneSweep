#pragma once

namespace ps {

struct AppContext;

class IScreen {
public:
    virtual ~IScreen() = default;
    virtual void Draw(AppContext& ctx) = 0;
};

} // namespace ps
