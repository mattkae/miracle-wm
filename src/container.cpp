/**
Copyright (C) 2024  Matthew Kosarek

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
**/

#define MIR_LOG_COMPONENT "container"

#include "container.h"
#include "leaf_container.h"
#include "node_common.h"
#include "parent_container.h"
#include "floating_container.h"

using namespace miracle;

Container::Container(std::shared_ptr<Container> const& parent) :
    parent { parent }
{
}

std::shared_ptr<LeafContainer> Container::as_leaf(std::shared_ptr<Container> const& container)
{
    return std::dynamic_pointer_cast<LeafContainer>(container);
}

std::shared_ptr<ParentContainer> Container::as_parent(std::shared_ptr<Container> const& container)
{
    return std::dynamic_pointer_cast<ParentContainer>(container);
}

std::shared_ptr<FloatingContainer> Container::as_floating(std::shared_ptr<Container> const& container)
{
    return std::dynamic_pointer_cast<FloatingContainer>(container);
}

bool Container::is_leaf()
{
    return as_leaf(shared_from_this()) != nullptr;
}

bool Container::is_lane()
{
    return as_parent(shared_from_this()) != nullptr;
}

std::weak_ptr<Container> Container::get_parent() const
{
    return parent;
}

namespace
{
bool has_neighbor(Container const* container, NodeLayoutDirection direction, size_t cannot_be_index)
{
    auto parent = container->get_parent().lock();
    if (!parent)
        return false;

    auto parent_container = Container::as_parent(parent);
    if (!parent_container)
        return false;

    if (parent_container->get_direction() != direction)
        return has_neighbor(parent_container.get(), direction, cannot_be_index);

    auto index = parent_container->get_index_of_node(container);
    return (parent_container->num_nodes() > 1 && index != cannot_be_index)
        || has_neighbor(parent_container.get(), direction, cannot_be_index);
}

bool has_right_neighbor(Container const* container)
{
    auto shared_parent = container->get_parent().lock();
    if (!shared_parent)
        return false;

    auto parent_container = Container::as_parent(shared_parent);
    if (!parent_container)
        return false;

    return has_neighbor(container, NodeLayoutDirection::horizontal, parent_container->num_nodes() - 1);
}

bool has_bottom_neighbor(Container const* container)
{
    auto shared_parent = container->get_parent().lock();
    if (!shared_parent)
        return false;

    auto parent_container = Container::as_parent(shared_parent);
    if (!parent_container)
        return false;

    return has_neighbor(container, NodeLayoutDirection::vertical, parent_container->num_nodes() - 1);
}

bool has_left_neighbor(Container const* container)
{
    auto shared_parent = container->get_parent().lock();
    if (!shared_parent)
        return false;
    return has_neighbor(container, NodeLayoutDirection::horizontal, 0);
}

bool has_top_neighbor(Container const* container)
{
    auto shared_parent = container->get_parent().lock();
    if (!shared_parent)
        return false;
    return has_neighbor(container, NodeLayoutDirection::vertical, 0);
}
}

std::array<bool, (size_t)Direction::MAX> Container::get_neighbors() const
{
    return {
        has_top_neighbor(this),
        has_left_neighbor(this),
        has_bottom_neighbor(this),
        has_right_neighbor(this)
    };
}