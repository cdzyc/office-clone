// oc::CommandStack — 撤销/重做骨架（Command 模式）
// 所有编辑操作封装为 Command；文档唯一可变入口（M2 接入 UI）。
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace oc {

class ICommand {
public:
    virtual ~ICommand() = default;
    virtual const std::string& name() const = 0;
    virtual void redo() = 0;
    virtual void undo() = 0;
    // merge：与上一命令合并（连续输入可合成一条，减少栈膨胀）
    virtual bool mergeWith(const ICommand&) { return false; }
};

class CommandStack {
public:
    explicit CommandStack(size_t limit = 1000) : limit_(limit) {}

    void execute(std::unique_ptr<ICommand> cmd) {
        cmd->redo();
        // 尝试与栈顶合并（如连续打字）
        if (!undo_.empty() && undo_.back()->mergeWith(*cmd)) return;
        undo_.push_back(std::move(cmd));
        redo_.clear();
        if (undo_.size() > limit_) undo_.erase(undo_.begin());
        dirty_ = true;
        ++rev_;
    }

    bool undo() {
        if (undo_.empty()) return false;
        undo_.back()->undo();
        redo_.push_back(std::move(undo_.back()));
        undo_.pop_back();
        dirty_ = true;
        ++rev_;
        return true;
    }

    bool redo() {
        if (redo_.empty()) return false;
        redo_.back()->redo();
        undo_.push_back(std::move(redo_.back()));
        redo_.pop_back();
        dirty_ = true;
        ++rev_;
        return true;
    }

    bool canUndo() const { return !undo_.empty(); }
    bool canRedo() const { return !redo_.empty(); }
    bool isDirty() const { return dirty_; }
    void markSaved() { dirty_ = false; }
    uint64_t revision() const { return rev_; }

    // 简单 lambda 命令工厂
    class LambdaCmd : public ICommand {
    public:
        LambdaCmd(std::string n, std::function<void()> u, std::function<void()> r)
            : name_(std::move(n)), undoFn_(std::move(u)), redoFn_(std::move(r)) {}
        const std::string& name() const override { return name_; }
        void redo() override { if (redoFn_) redoFn_(); }
        void undo() override { if (undoFn_) undoFn_(); }
    private:
        std::string name_;
        std::function<void()> undoFn_, redoFn_;
    };

    static std::unique_ptr<ICommand> makeCmd(std::string name,
                                             std::function<void()> undo,
                                             std::function<void()> redo) {
        return std::make_unique<LambdaCmd>(std::move(name), std::move(undo), std::move(redo));
    }

private:
    std::vector<std::unique_ptr<ICommand>> undo_, redo_;
    size_t limit_;
    bool dirty_ = false;
    uint64_t rev_ = 0;
};

} // namespace oc
