# -*- coding: utf-8 -*-
# This file is part of Dyko
# Copyright © 2008-2009 Kozea
#
# This library is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This library is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with Kalamar.  If not, see <http://www.gnu.org/licenses/>.

"""
Query helpers for the Alchemy access point.

"""

from ... import query
from ...request import And, Or, Not

from sqlalchemy.sql import expression


def query_chain_to_alchemy(self, alchemy_query, access_point, properties):
    """Monkey-patched method on QueryChain to convert to alchemy."""
    for sub_query in self.queries:
        alchemy_query = sub_query.to_alchemy(
            alchemy_query, access_point, properties)
        properties = sub_query.validate(access_point.site, properties)
    return alchemy_query


def query_chain_validator(self, access_point, properties):
    cants = []
    cans = []
    for sub_query in self.queries:
        managed, not_managed = sub_query.alchemy_validate(
            access_point, properties)
        if not_managed is not None:
            cants.append(not_managed)         
        if managed is not None:
            cans.append(managed)
        properties = sub_query.validate(access_point.site, properties)
    query_can = query.QueryChain(cans) if cans and not cants else None
    return query_can, query.QueryChain(cants)


def standard_validator(self, access_point, properties):
    return self, None


def query_filter_to_alchemy(self, alchemy_query, access_point, properties):
    """Monkey-patched method on QueryFilter to convert to alchemy."""
    access_points = access_point.site.access_points

    def to_alchemy_condition(condition):
        """Converts a kalamar condition to an sqlalchemy condition."""
        if isinstance(condition, (And, Or, Not)):
            alchemy_conditions = tuple(
                to_alchemy_condition(sub_condition)
                for sub_condition in condition.sub_requests)
            return condition.alchemy_function(*alchemy_conditions)
        else:
            column = properties[condition.property.name].column
            if condition.operator == "=":
                return column == condition.value
            else:
                return column.op(condition.operator)(condition.value)

    def build_join(tree, properties, alchemy_query):
        for name, values in tree.items():
            prop = properties[name]
            if prop.remote_ap:
                remote_ap = access_points[prop.remote_ap]
                join_col1 = alchemy_query.corresponding_column(prop.column)
                if prop.relation == "many-to-one":
                    join_col2 = alchemy_query.corresponding_column(
                        remote_ap.properties[remote_ap.identity_properties[0]])
                    alchemy_query = alchemy_query.select_from(
                        access_points[remote_ap._table]).where(
                        join_col1 == join_col2)
                    build_join(values, remote_ap.properties, alchemy_query)
        return alchemy_query

    alchemy_query = build_join(
        self.condition.properties_tree, properties, alchemy_query)
    alchemy_query = alchemy_query.where(to_alchemy_condition(self.condition))
    return alchemy_query


def query_filter_validator(self, access_point, properties):
    from . import Alchemy
    cond_tree = self.condition.properties_tree
    access_points = access_point.site.access_points

    def inner_manage(name, values, properties):
        if name not in properties:
            return False
        elif properties[name].remote_ap:
            remote_ap = access_points[properties[name].remote_ap]
            return isinstance(remote_ap, Alchemy) and \
                all(inner_manage(name, values, remote_ap.properties)
                    for name, values in cond_tree.items())
        else: 
            return True

    if all(inner_manage(name, values, properties)
           for name, values in cond_tree.items()):
        return self, None
    else:
        return None, self


def query_select_to_alchemy(self, alchemy_query, access_point, properties):
    """Monkey-patched method on QuerySelect to convert to alchemy."""
    access_points = access_point.site.access_points
    for name, sub_select in self.sub_selects.items():
        remote_ap = access_points[properties[name].remote_ap]
        remote_property = remote_ap.properties[properties[name].remote_property]
        col1 = properties[name].column
        col2 = remote_property.column
        alchemy_query = alchemy_query.select_from(
            remote_ap._table).where(col1 == col2)
        alchemy_query = sub_select.to_alchemy(
            alchemy_query,  access_point, remote_ap.properties)
    for name, value in self.mapping.items():
        alchemy_query.append_column(properties[value.name].column.label(name))
    return alchemy_query


def query_select_validator(self, access_point, properties):
    from . import Alchemy
    access_points = access_point.site.access_points

    def isvalid(select, properties):
        for name, sub_select in select.sub_selects.items():
            remote_ap = access_points[properties[name].remote_ap]
            if not isinstance(remote_ap, Alchemy) or \
                    not isvalid(sub_select, remote_ap.properties):
                return False
        return True

    if isvalid(self, properties):
        return self, None
    else:
        return None, self


def query_distinct_to_alchemy(self, alchemy_query, access_point, properties):
    return alchemy_query.distinct()
    

def query_range_to_alchemy(self, alchemy_query, access_point, properties):
    if self.range.start:
        alchemy_query = alchemy_query.offset(self.range.start)
    if self.range.stop:
        alchemy_query = alchemy_query.limit(
            self.range.stop - (self.range.start or 0))
    return alchemy_query


def query_order_to_alchemy(self, alchemy_query, access_point, properties):
    for key, order in self.orderbys:
        alchemy_query = alchemy_query.order_by(
            expression.asc(key) if order else expression.desc(key))
    return alchemy_query


query.QueryChain.alchemy_validate = query_chain_validator
query.QueryDistinct.alchemy_validate = standard_validator
query.QueryFilter.alchemy_validate = query_filter_validator
query.QueryOrder.alchemy_validate = standard_validator
query.QueryRange.alchemy_validate = standard_validator
query.QuerySelect.alchemy_validate = query_select_validator

query.QueryChain.to_alchemy = query_chain_to_alchemy
query.QueryDistinct.to_alchemy = query_distinct_to_alchemy
query.QueryFilter.to_alchemy = query_filter_to_alchemy
query.QueryOrder.to_alchemy = query_order_to_alchemy
query.QueryRange.to_alchemy = query_range_to_alchemy
query.QuerySelect.to_alchemy = query_select_to_alchemy