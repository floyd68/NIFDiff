// NifItem.h - Qt-free replacement for src/data/nifitem.h
//
// Scope note: the original NifItem is the generic tree-node type behind
// NifModel's QAbstractItemModel (name/type/arg/arr1/arr2/cond/ver1/ver2
// strings driven by nif.xml, plus a NifValue payload and QVector<NifItem*>
// children, refcounted via QSharedData/QPointer so QModelIndex can hold
// stable internalPointer()s). The lite viewer's render path does not need a
// generic per-field tree (see NifDocument.h), so this port keeps only what a
// simple "block field browser" (used by NifCompareControlPanel's optional
// debug/info display) actually needs: an owning tree of name -> NifValue
// pairs, built with std::unique_ptr instead of QSharedData/QPointer.
#pragma once

#include "NifValue.h"
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace nsk
{

class NifItem
{
public:
    explicit NifItem(std::string name, NifItem* parent = nullptr)
        : m_name(std::move(name)), m_parent(parent)
    {
    }

    const std::string& name() const { return m_name; }
    void setName(std::string name) { m_name = std::move(name); }

    const NifValue& value() const { return m_value; }
    void setValue(NifValue v) { m_value = std::move(v); }

    NifItem* parent() const { return m_parent; }

    NifItem* addChild(std::string name)
    {
        m_children.push_back(std::make_unique<NifItem>(std::move(name), this));
        return m_children.back().get();
    }

    int childCount() const { return static_cast<int>(m_children.size()); }
    NifItem* child(int row) const { return (row >= 0 && row < childCount()) ? m_children[static_cast<size_t>(row)].get() : nullptr; }

    NifItem* findChild(std::string_view name) const
    {
        for (auto& c : m_children)
            if (c->name() == name)
                return c.get();
        return nullptr;
    }

    int row() const
    {
        if (!m_parent) return 0;
        for (size_t i = 0; i < m_parent->m_children.size(); ++i)
            if (m_parent->m_children[i].get() == this)
                return static_cast<int>(i);
        return 0;
    }

private:
    std::string m_name;
    NifValue m_value;
    NifItem* m_parent;
    std::vector<std::unique_ptr<NifItem>> m_children;
};

} // namespace nsk
