/*
	This file is part of solidity.

	solidity is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	solidity is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with solidity.  If not, see <http://www.gnu.org/licenses/>.
*/
// SPDX-License-Identifier: GPL-3.0

#pragma once

#include <libyul/backends/evm/ControlFlowGraph.h>
#include <libyul/Exceptions.h>

#include <libsolutil/Visitor.h>

#include <range/v3/algorithm/all_of.hpp>
#include <range/v3/view/enumerate.hpp>
#include <range/v3/view/iota.hpp>
#include <range/v3/view/reverse.hpp>
#include <range/v3/view/take.hpp>

namespace solidity::yul
{

inline std::string stackSlotToString(StackSlot const& _slot)
{
	return std::visit(util::GenericVisitor{
		[](FunctionCallReturnLabelSlot const& _ret) -> std::string { return "RET[" + _ret.call.get().functionName.name.str() + "]"; },
		[](FunctionReturnLabelSlot const&) -> std::string { return "RET"; },
		[](VariableSlot const& _var) { return _var.variable.get().name.str(); },
		[](LiteralSlot const& _lit) { return util::toCompactHexWithPrefix(_lit.value); },
		[](TemporarySlot const& _tmp) -> std::string { return "TMP[" + _tmp.call.get().functionName.name.str() + ", " + std::to_string(_tmp.index) + "]"; },
		[](JunkSlot const&) -> std::string { return "JUNK"; }
	}, _slot);
}

inline std::string stackToString(Stack const& _stack)
{
	std::string result("[ ");
	for (auto const& slot: _stack)
		result += stackSlotToString(slot) + ' ';
	result += ']';
	return result;
}


// Abstraction of stack shuffling operations. Can be defined as actual concept once we switch to C++20.
/*
template<typename ShuffleOperations>
concept ShuffleOperationConcept = requires(ShuffleOperations ops, size_t sourceOffset, size_t targetOffset, size_t depth) {
	// Returns true, iff the current slot at sourceOffset in source layout is a suitable slot at targetOffset.
	{ ops.isCompatible(sourceOffset, targetOffset) } -> std::convertible_to<bool>;
	// Returns true, iff the slots at the two given source offsets are identical.
	{ ops.souceIsSame(sourceOffset, sourceOffset) } -> std::convertible_to<bool>;
	// Returns a positive integer n, if the slot at the given source offset needs n more copies.
	// Returns a negative integer -n, if the slot at the given source offsets occurs n times too many.
	// Returns zero if the amount of occurrences, in the current source layout, of the slot at the given source offset matches the desired amount of occurrences in the target.
	{ ops.sourceMultiplicity(sourceOffset) } -> std::convertible_to<int>;
	// Returns a positive integer n, if the slot at the given target offset needs n more copies.
	// Returns a negative integer -n, if the slot at the given target offsets occurs n times too many.
	// Returns zero if the amount of occurrences, in the current source layout, of the slot at the given target offset matches the desired amount of occurrences in the target.
	{ ops.targetMultiplicity(targetOffset) } -> std::convertible_to<int>;
	// Returns true, iff any slot is compatible with the given target offset.
	{ ops.targetIsArbitrary(targetOffset) } -> std::convertible_to<bool>;
	// Returns the number of slots in the source layout.
	{ ops.sourceSize() } -> std::convertible_to<size_t>;
	// Returns the number of slots in the target layout.
	{ ops.targetSize() } -> std::convertible_to<size_t>;
	// Swaps the top most slot in the source with the slot at depth.
	{ ops.swap(depth) };
	// Pops the top most slot in the source.
	{ ops.pop() };
	// Dups or pushes the slot that is supposed to end up at the given target offset.
	{ ops.pushOrDupTarget(targetOffset) };
};
*/
/// Helper class that can perform shuffling of a source stack layout to a target stack layout via
/// abstracted shuffle operations.
template</*ShuffleOperationConcept*/ typename ShuffleOperations>
class Shuffler
{
public:
	/// Executes the stack shuffling operations. Instantiates an instance of ShuffleOperations
	/// in each iteration. Each iteration performs exactly one operation that modifies the stack.
	/// After shuffle, all slots in the source layout are guaranteed to be compatible to the slots
	/// at the same target offset, but there may be additional slots in the target that are not
	/// pushed/dupped yet.
	template<typename... Args>
	static void shuffle(Args&&... args)
	{
		bool needsMoreShuffling = true;
		// The shuffling algorithm should always terminate in polynomial time, but we provide a limit
		// in case it does not terminate due to a bug.
		size_t iterationCount = 0;
		while (iterationCount < 1000 && (needsMoreShuffling = shuffleStep(std::forward<Args>(args)...)))
			++iterationCount;
		yulAssert(!needsMoreShuffling, "Could not create stack layout after 1000 iterations.");
	}
private:
	/// Performs a single stack operation, transforming the source layout closer to the target layout.
	template<typename... Args>
	static bool shuffleStep(Args&&... args)
	{
		ShuffleOperations ops{std::forward<Args>(args)...};

		// Terminates, if all slots in the source are compatible with the target.
		// Note that there may still be more slots in the target.
		if (ranges::all_of(
			ranges::views::iota(0u, ops.sourceSize()),
			[&](size_t _index) { return ops.isCompatible(_index, _index); }
		))
			return false;

		size_t sourceTop = ops.sourceSize() - 1;
		// If we no longer need the current stack top, we pop it, unless we need an arbitrary slot at this position
		// in the target.
		if (
			ops.sourceMultiplicity(sourceTop) < 0 &&
			!(ops.targetSize() >= ops.sourceSize() && ops.targetIsArbitrary(sourceTop))
		)
		{
			ops.pop();
			return true;
		}

		yulAssert(ops.targetSize() > 0, "");

		// If the top is not supposed to be exactly what is on top right now, try to find a lower position to swap it to.
		if (!ops.isCompatible(sourceTop, sourceTop) || ops.targetIsArbitrary(sourceTop))
			for (size_t offset: ranges::views::iota(0u, std::min(ops.sourceSize(), ops.targetSize())))
				// It makes sense to swap to a lower position, if
				if (
					!ops.isCompatible(offset, offset) && // The lower slot is not already in position.
					!ops.sourceIsSame(offset, sourceTop) && // We would not just swap identical slots.
					ops.isCompatible(sourceTop, offset) // The lower position wants to have this slot.
				)
				{
					ops.swap(ops.sourceSize() - offset - 1);
					return true;
				}

		auto bringUpTargetSlot = [&](size_t _targetOffset) {
			std::list<size_t> toVisit{_targetOffset};
			std::set<size_t> visited;

			while (!toVisit.empty())
			{
				auto offset = *toVisit.begin();
				toVisit.erase(toVisit.begin());
				visited.emplace(offset);
				if (ops.targetMultiplicity(offset) > 0)
				{
					ops.pushOrDupTarget(offset);
					return;
				}
				// The desired target slot must already be somewhere else on stack right now.
				for (auto nextOffset: ranges::views::iota(0u, std::min(ops.sourceSize(), ops.targetSize())))
					if (
						!ops.isCompatible(nextOffset, nextOffset) &&
						ops.isCompatible(nextOffset, offset)
					)
					if (!visited.count(nextOffset))
						toVisit.emplace_back(nextOffset);
			}
			yulAssert(false, "");
		};

		// If a lower slot should be removed, try to bring up the slot that should end up there and bring it up.
		// Note that after the cases above, there will always be a target slot to duplicate in this case.
		for (size_t offset: ranges::views::iota(0u, ops.sourceSize()))
			if (
				!ops.isCompatible(offset, offset) && // The lower slot is not already in position.
				ops.sourceMultiplicity(offset) < 0 && // We have too many copies of this slot.
				offset <= ops.targetSize() && // There is a target slot at this position.
				!ops.targetIsArbitrary(offset) // And that target slot is not arbitrary.
			)
			{
				bringUpTargetSlot(offset);
				return true;
			}

		// At this point we want to keep all slots.
		for (size_t i = 0; i < ops.sourceSize(); ++i)
			yulAssert(ops.sourceMultiplicity(i) >= 0, "");
		yulAssert(ops.sourceSize() <= ops.targetSize(), "");

		// If the top is not in position, try to find a slot that wants to be at the top and swap it up.
		if (!ops.isCompatible(sourceTop, sourceTop))
			for (size_t sourceOffset: ranges::views::iota(0u, ops.sourceSize()))
				if (
					!ops.isCompatible(sourceOffset, sourceOffset) &&
					ops.isCompatible(sourceOffset, sourceTop)
				)
				{
					ops.swap(ops.sourceSize() - sourceOffset - 1);
					return true;
				}

		// If we still need more slots, produce a suitable one.
		if (ops.sourceSize() < ops.targetSize())
		{
			bringUpTargetSlot(ops.sourceSize());
			return true;
		}

		// The stack has the correct size, each slot has the correct number of copies and the top is in position.
		yulAssert(ops.sourceSize() == ops.targetSize(), "");
		size_t size = ops.sourceSize();
		for (size_t i = 0; i < ops.sourceSize(); ++i)
			yulAssert(ops.sourceMultiplicity(i) == 0 && (ops.targetIsArbitrary(i) || ops.targetMultiplicity(i) == 0), "");
		yulAssert(ops.isCompatible(sourceTop, sourceTop), "");

		// If we find a lower slot that is out of position, but also compatible with the top, swap that up.
		for (size_t offset: ranges::views::iota(0u, size))
			if (!ops.isCompatible(offset, offset) && ops.isCompatible(sourceTop, offset))
			{
				ops.swap(size - offset - 1);
				return true;
			}
		// Swap up any slot that is still out of position.
		for (size_t offset: ranges::views::iota(0u, size))
			if (!ops.isCompatible(offset, offset) && !ops.sourceIsSame(offset, sourceTop))
			{
				ops.swap(size - offset - 1);
				return true;
			}
		yulAssert(false, "");
	}
};


/// Transforms @a _currentStack to @a _targetStack, invoking the provided shuffling operations.
/// Modifies @a _currentStack itself after each invocation of the shuffling operations.
template<typename Swap, typename PushOrDup, typename Pop>
void createStackLayout(Stack& _currentStack, Stack const& _targetStack, Swap _swap, PushOrDup _pushOrDup, Pop _pop)
{
	struct ShuffleOperations
	{
		Stack& currentStack;
		Stack const& targetStack;
		Swap swapCallback;
		PushOrDup pushOrDupCallback;
		Pop popCallback;
		std::map<StackSlot, int> multiplicity;
		ShuffleOperations(
			Stack& _currentStack,
			Stack const& _targetStack,
			Swap _swap,
			PushOrDup _pushOrDup,
			Pop _pop
		):
			currentStack(_currentStack),
			targetStack(_targetStack),
			swapCallback(_swap),
			pushOrDupCallback(_pushOrDup),
			popCallback(_pop)
		{
			for (auto const& slot: currentStack)
				--multiplicity[slot];
			for (auto&& [offset, slot]: targetStack | ranges::views::enumerate)
				if (std::holds_alternative<JunkSlot>(slot) && offset < currentStack.size())
					++multiplicity[currentStack.at(offset)];
				else
					++multiplicity[slot];
		}
		bool isCompatible(size_t _source, size_t _target)
		{
			return
				_source < currentStack.size() &&
				_target < targetStack.size() &&
				(
					std::holds_alternative<JunkSlot>(targetStack.at(_target)) ||
					currentStack.at(_source) == targetStack.at(_target)
				);
		}
		bool sourceIsSame(size_t _lhs, size_t _rhs) { return currentStack.at(_lhs) == currentStack.at(_rhs); }
		int sourceMultiplicity(size_t _offset) { return multiplicity.at(currentStack.at(_offset)); }
		int targetMultiplicity(size_t _offset) { return multiplicity.at(targetStack.at(_offset)); }
		bool targetIsArbitrary(size_t offset)
		{
			return offset < targetStack.size() && std::holds_alternative<JunkSlot>(targetStack.at(offset));
		}
		void swap(size_t _i)
		{
			swapCallback(static_cast<unsigned>(_i));
			std::swap(currentStack.at(currentStack.size() -  _i - 1), currentStack.back());
		}
		size_t sourceSize() { return currentStack.size(); }
		size_t targetSize() { return targetStack.size(); }
		void pop()
		{
			popCallback();
			currentStack.pop_back();
		}
		void pushOrDupTarget(size_t _offset)
		{
			auto const& targetSlot = targetStack.at(_offset);
			pushOrDupCallback(targetSlot);
			currentStack.push_back(targetSlot);
		}
	};

	Shuffler<ShuffleOperations>::shuffle(_currentStack, _targetStack, _swap, _pushOrDup, _pop);

	while (_currentStack.size() < _targetStack.size())
	{
		_pushOrDup(_targetStack.at(_currentStack.size()));
		_currentStack.push_back(_targetStack.at(_currentStack.size()));
	}

	yulAssert(_currentStack.size() == _targetStack.size(), "");
	for (auto&& [current, target]: ranges::zip_view(_currentStack, _targetStack))
		if (std::holds_alternative<JunkSlot>(target))
			current = JunkSlot{};
		else
			yulAssert(current == target, "");
}

}
