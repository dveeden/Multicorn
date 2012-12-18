CREATE EXTENSION multicorn;
CREATE server multicorn_srv foreign data wrapper multicorn options (
    wrapper 'multicorn.testfdw.TestForeignDataWrapper'
);
CREATE user mapping for postgres server multicorn_srv options (usermapping 'test');

CREATE foreign table testmulticorn (
    test1 character varying,
    test2 character varying
) server multicorn_srv options (
    option1 'option1'
);

insert into testmulticorn(test1, test2) VALUES ('lol', 'lol2');

update testmulticorn set test1 = 'lol';

delete from testmulticorn where test2 = 'lol2';

CREATE foreign table testmulticorn_write (
    test1 character varying,
    test2 character varying
) server multicorn_srv options (
    option1 'option1',
	rowid_column 'test1'
);

insert into testmulticorn_write(test1, test2) VALUES ('lol', 'lol2');

update testmulticorn_write set test1 = 'lol';

delete from testmulticorn_write where test2 = 'lol2';

DROP EXTENSION multicorn cascade;
