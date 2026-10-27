#pragma once
#include "task_store.hpp"

namespace asterion::research {

// What Research's task kinds mean to its task store.
const tasks::TaskDomain& task_domain();

// Research's catalogue of its fixed task inputs and results.
resource::proto::Catalogue task_resources(
    const tasks::Store&,
    const std::string& node,
    const resource::proto::CatalogueQuery&);
// The tasks bound to one research project's plan, newest last.

} // namespace asterion::research
